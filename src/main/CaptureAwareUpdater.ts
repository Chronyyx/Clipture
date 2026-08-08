import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { mkdir, open, readFile, writeFile } from "node:fs/promises";
import type { FileHandle } from "node:fs/promises";
import { constants as osConstants, getPriority, setPriority } from "node:os";
import { join } from "node:path";
import { gunzipSync, gzipSync } from "node:zlib";
import type { Session } from "electron";
import {
  CancellationError,
  CURRENT_APP_INSTALLER_FILE_NAME
} from "builder-util-runtime";
import type { CancellationToken } from "builder-util-runtime";
import { NsisUpdater } from "electron-updater";
import type { DownloadUpdateOptions } from "electron-updater/out/AppUpdater";
import type { Provider } from "electron-updater/out/providers/Provider";
import type { Logger, ResolvedUpdateFileInfo } from "electron-updater/out/types";
import {
  boundedUpdateWriteSize,
  buildUpdateTransferPlan,
  maximumUpdateWriteBytes,
  UpdateWritePacer,
  UpdateWriteRateController
} from "./UpdateTransferPolicy";
import type {
  UpdateBlockMap,
  UpdateCapturePressure,
  UpdateTransferOperation,
  UpdateTransferPlan
} from "./UpdateTransferPolicy";

function requestHeadersForFetch(headers: NodeJS.Dict<string | string[] | number>): Record<string, string> {
  const result: Record<string, string> = {};
  for (const [name, value] of Object.entries(headers)) {
    if (value === undefined) continue;
    if (name.toLowerCase() === "host" || name.toLowerCase() === "content-length") continue;
    result[name] = Array.isArray(value) ? value.join(", ") : String(value);
  }
  return result;
}

function mibPerSecond(bytesPerSecond: number): string {
  return (bytesPerSecond / (1024 * 1024)).toFixed(1);
}

export class CaptureAwareUpdateTransfer {
  private readonly rateController = new UpdateWriteRateController();
  private readonly pacer = new UpdateWritePacer(this.rateController);
  private session: Session | undefined;
  private logger: Logger | undefined;
  private pressureTimer: ReturnType<typeof setInterval> | undefined;
  private pressureRefreshInFlight = false;
  private previousProcessPriority: number | undefined;
  private appliedNetworkRate = 0;
  private startedAtMs = 0;
  private writtenBytes = 0;
  private maximumWriteBytes = 0;
  private pacingWaitMs = 0;
  private pressureTransitions = 0;
  private rateAdjustments = 0;

  constructor(
    private readonly capturePressure: () => UpdateCapturePressure,
    private readonly refreshCapturePressure: () => Promise<void>
  ) {}

  start(session: Session, logger: Logger): void {
    this.stop();
    this.session = session;
    this.logger = logger;
    this.rateController.reset();
    this.pacer.reset();
    this.startedAtMs = Date.now();
    this.writtenBytes = 0;
    this.maximumWriteBytes = 0;
    this.pacingWaitMs = 0;
    this.pressureTransitions = 0;
    this.rateAdjustments = 0;
    this.syncCapturePressure();
    this.applyNetworkRate(true);

    try {
      this.previousProcessPriority = getPriority(process.pid);
      setPriority(process.pid, osConstants.priority.PRIORITY_BELOW_NORMAL);
    } catch {
      this.previousProcessPriority = undefined;
    }

    this.pressureTimer = setInterval(() => {
      if (this.pressureRefreshInFlight) return;
      this.pressureRefreshInFlight = true;
      void this.refreshCapturePressure()
        .catch(() => undefined)
        .finally(() => {
          this.pressureRefreshInFlight = false;
          this.syncCapturePressure();
        });
    }, 750);

    this.logger.info(
      `[update-transfer] started rateMiBps=${mibPerSecond(this.rateController.currentBytesPerSecond())}`
    );
  }

  stop(): void {
    if (this.pressureTimer) clearInterval(this.pressureTimer);
    this.pressureTimer = undefined;
    this.pressureRefreshInFlight = false;

    if (this.session) {
      try {
        this.session.disableNetworkEmulation();
      } catch {
        // The dedicated updater session may already be shutting down.
      }
    }

    if (this.previousProcessPriority !== undefined) {
      try {
        setPriority(process.pid, this.previousProcessPriority);
      } catch {
        // Process priority is best-effort and must never affect update completion.
      }
    }

    if (this.logger && this.startedAtMs > 0) {
      this.logger.info(
        `[update-transfer] finished ms=${Date.now() - this.startedAtMs}` +
          ` bytes=${this.writtenBytes}` +
          ` maxWriteBytes=${this.maximumWriteBytes}` +
          ` pacingWaitMs=${Math.round(this.pacingWaitMs)}` +
          ` pressureTransitions=${this.pressureTransitions}` +
          ` rateAdjustments=${this.rateAdjustments}`
      );
    }

    this.session = undefined;
    this.logger = undefined;
    this.previousProcessPriority = undefined;
    this.appliedNetworkRate = 0;
    this.startedAtMs = 0;
  }

  async write(handle: FileHandle, data: Uint8Array, position: number): Promise<number> {
    let sourceOffset = 0;
    let outputPosition = position;
    while (sourceOffset < data.byteLength) {
      this.syncCapturePressure();
      const writeBytes = boundedUpdateWriteSize(data.byteLength - sourceOffset);
      const chunk = data.subarray(sourceOffset, sourceOffset + writeBytes);
      this.pacingWaitMs += await this.pacer.pace(writeBytes);

      const writeStartedAt = performance.now();
      let chunkOffset = 0;
      while (chunkOffset < chunk.byteLength) {
        const result = await handle.write(
          chunk,
          chunkOffset,
          chunk.byteLength - chunkOffset,
          outputPosition + chunkOffset
        );
        if (result.bytesWritten <= 0) throw new Error("Update writer made no forward progress");
        chunkOffset += result.bytesWritten;
      }
      const writeDurationMs = performance.now() - writeStartedAt;
      if (this.rateController.observeWrite(writeBytes, writeDurationMs)) {
        this.rateAdjustments += 1;
        this.applyNetworkRate();
      }

      this.writtenBytes += writeBytes;
      this.maximumWriteBytes = Math.max(this.maximumWriteBytes, writeBytes);
      sourceOffset += writeBytes;
      outputPosition += writeBytes;
    }
    return outputPosition;
  }

  private syncCapturePressure(): void {
    const previousPressure = this.rateController.currentPressure();
    const nextPressure = this.capturePressure();
    const rateChanged = this.rateController.observePressure(nextPressure);
    if (previousPressure !== nextPressure) {
      this.pressureTransitions += 1;
      this.logger?.info(
        `[update-transfer] pressure=${nextPressure}` +
          ` rateMiBps=${mibPerSecond(this.rateController.currentBytesPerSecond())}`
      );
    }
    if (rateChanged) {
      this.rateAdjustments += 1;
      this.applyNetworkRate();
    }
  }

  private applyNetworkRate(force = false): void {
    if (!this.session) return;
    const rate = this.rateController.currentBytesPerSecond();
    if (!force && rate === this.appliedNetworkRate) return;
    this.session.enableNetworkEmulation({
      offline: false,
      latency: 0,
      downloadThroughput: rate,
      uploadThroughput: 0
    });
    this.appliedNetworkRate = rate;
  }
}

type UpdateProgress = {
  total: number;
  delta: number;
  transferred: number;
  percent: number;
  bytesPerSecond: number;
};

class DifferentialProgressTracker {
  private readonly startedAtMs = Date.now();
  private lastUpdateAtMs = this.startedAtMs;
  private lastTransferred = 0;

  constructor(
    private readonly total: number,
    private readonly emit: (progress: UpdateProgress) => void
  ) {}

  update(transferred: number, force = false): void {
    const now = Date.now();
    if (!force && now - this.lastUpdateAtMs < 500) return;
    const elapsedSeconds = Math.max(0.001, (now - this.startedAtMs) / 1000);
    const boundedTransferred = Math.min(this.total, Math.max(0, transferred));
    this.emit({
      total: this.total,
      delta: boundedTransferred - this.lastTransferred,
      transferred: boundedTransferred,
      percent: this.total > 0 ? boundedTransferred / this.total * 100 : 100,
      bytesPerSecond: Math.round(boundedTransferred / elapsedSeconds)
    });
    this.lastTransferred = boundedTransferred;
    this.lastUpdateAtMs = now;
  }
}

export class CaptureAwareNsisUpdater extends NsisUpdater {
  constructor(private readonly transfer: CaptureAwareUpdateTransfer) {
    super();
  }

  protected override async differentialDownloadInstaller(
    fileInfo: ResolvedUpdateFileInfo,
    downloadUpdateOptions: DownloadUpdateOptions,
    installerPath: string,
    provider: Provider<any>,
    oldInstallerFileName: string
  ): Promise<boolean> {
    try {
      const helper = this.downloadedUpdateHelper;
      if (!helper) throw new Error("Updater cache has not been initialized");

      const oldInstallerPath = join(
        helper.cacheDir,
        oldInstallerFileName || CURRENT_APP_INSTALLER_FILE_NAME
      );
      if (!existsSync(oldInstallerPath)) {
        throw new Error("Previous installer is not cached yet");
      }

      const blockmapUrls = await provider.getBlockMapFiles(
        fileInfo.url,
        this.app.version,
        downloadUpdateOptions.updateInfoAndProvider.info.version,
        this.previousBlockmapBaseUrlOverride
      );
      this._logger.info(
        `Download block maps (old: "${blockmapUrls[0]}", new: "${blockmapUrls[1]}")`
      );

      const newBlockMap = await this.downloadBlockMap(
        blockmapUrls[1],
        downloadUpdateOptions.requestHeaders,
        downloadUpdateOptions.cancellationToken
      );
      const cachedOldBlockMapPath = join(helper.cacheDir, "current.blockmap");
      let oldBlockMap: UpdateBlockMap;
      if (existsSync(cachedOldBlockMapPath)) {
        oldBlockMap = this.parseBlockMap(await readFile(cachedOldBlockMapPath), cachedOldBlockMapPath);
      } else {
        oldBlockMap = await this.downloadBlockMap(
          blockmapUrls[0],
          downloadUpdateOptions.requestHeaders,
          downloadUpdateOptions.cancellationToken
        );
      }

      const plan = buildUpdateTransferPlan(oldBlockMap, newBlockMap);
      if (fileInfo.info.size !== undefined && fileInfo.info.size !== plan.totalBytes) {
        throw new Error(
          `Blockmap output size ${plan.totalBytes} does not match update size ${fileInfo.info.size}`
        );
      }
      this._logger.info(
        `[update-transfer] differential total=${plan.totalBytes}` +
          ` copy=${plan.copyBytes}` +
          ` download=${plan.downloadBytes}` +
          ` operations=${plan.operations.length}`
      );

      await this.reconstructInstaller(
        plan,
        oldInstallerPath,
        installerPath,
        fileInfo,
        fileInfo.url,
        downloadUpdateOptions
      );

      await mkdir(helper.cacheDirForPendingUpdate, { recursive: true });
      await writeFile(
        join(helper.cacheDirForPendingUpdate, "current.blockmap"),
        gzipSync(JSON.stringify(newBlockMap))
      );
      return false;
    } catch (error) {
      this._logger.error(
        `Cannot download differentially, fallback to paced full download: ${
          error instanceof Error ? error.stack || error.message : String(error)
        }`
      );
      return true;
    }
  }

  private async reconstructInstaller(
    plan: UpdateTransferPlan,
    oldInstallerPath: string,
    newInstallerPath: string,
    fileInfo: ResolvedUpdateFileInfo,
    updateUrl: URL,
    downloadUpdateOptions: DownloadUpdateOptions
  ): Promise<void> {
    const oldInstaller = await open(oldInstallerPath, "r");
    const newInstaller = await open(newInstallerPath, "w");
    const hash = createHash("sha512");
    const progress = new DifferentialProgressTracker(
      plan.totalBytes,
      (value) => this.emit("download-progress", value)
    );
    let outputPosition = 0;

    try {
      for (const operation of plan.operations) {
        if (downloadUpdateOptions.cancellationToken.cancelled) throw new CancellationError();
        if (operation.kind === "copy") {
          outputPosition = await this.copyOperation(
            operation,
            oldInstaller,
            newInstaller,
            outputPosition,
            hash,
            progress
          );
        } else {
          outputPosition = await this.downloadOperation(
            operation,
            newInstaller,
            outputPosition,
            hash,
            progress,
            updateUrl,
            downloadUpdateOptions
          );
        }
      }

      if (outputPosition !== plan.totalBytes) {
        throw new Error(`Differential update wrote ${outputPosition} of ${plan.totalBytes} bytes`);
      }
      const digest = hash.digest("base64");
      if (digest !== fileInfo.info.sha512) {
        throw new Error("Differential update SHA-512 checksum does not match latest.yml");
      }
      progress.update(outputPosition, true);
    } finally {
      await Promise.allSettled([oldInstaller.close(), newInstaller.close()]);
    }
  }

  private async copyOperation(
    operation: UpdateTransferOperation,
    oldInstaller: FileHandle,
    newInstaller: FileHandle,
    initialOutputPosition: number,
    hash: ReturnType<typeof createHash>,
    progress: DifferentialProgressTracker
  ): Promise<number> {
    const buffer = Buffer.allocUnsafe(maximumUpdateWriteBytes);
    let sourcePosition = operation.start;
    let outputPosition = initialOutputPosition;
    while (sourcePosition < operation.end) {
      const requestedBytes = Math.min(buffer.byteLength, operation.end - sourcePosition);
      const result = await oldInstaller.read(buffer, 0, requestedBytes, sourcePosition);
      if (result.bytesRead <= 0) throw new Error("Previous installer ended inside a copied block");
      const chunk = buffer.subarray(0, result.bytesRead);
      hash.update(chunk);
      outputPosition = await this.transfer.write(newInstaller, chunk, outputPosition);
      sourcePosition += result.bytesRead;
      progress.update(outputPosition);
    }
    return outputPosition;
  }

  private async downloadOperation(
    operation: UpdateTransferOperation,
    newInstaller: FileHandle,
    initialOutputPosition: number,
    hash: ReturnType<typeof createHash>,
    progress: DifferentialProgressTracker,
    updateUrl: URL,
    downloadUpdateOptions: DownloadUpdateOptions
  ): Promise<number> {
    const response = await this.fetchWithCancellation(
      updateUrl,
      {
        ...downloadUpdateOptions.requestHeaders,
        Range: `bytes=${operation.start}-${operation.end - 1}`
      },
      downloadUpdateOptions.cancellationToken
    );
    if (response.status !== 206) {
      throw new Error(`Update server returned HTTP ${response.status} for a byte-range request`);
    }
    if (!response.body) throw new Error("Update byte-range response has no body");

    const reader = response.body.getReader();
    let outputPosition = initialOutputPosition;
    let receivedBytes = 0;
    while (true) {
      const result = await reader.read();
      if (result.done) break;
      const chunk = result.value;
      receivedBytes += chunk.byteLength;
      if (receivedBytes > operation.end - operation.start) {
        throw new Error("Update byte-range response exceeded the requested block");
      }
      hash.update(chunk);
      outputPosition = await this.transfer.write(newInstaller, chunk, outputPosition);
      progress.update(outputPosition);
    }
    if (receivedBytes !== operation.end - operation.start) {
      throw new Error(
        `Update byte-range response returned ${receivedBytes} of ${operation.end - operation.start} bytes`
      );
    }
    return outputPosition;
  }

  private async downloadBlockMap(
    url: URL,
    headers: NodeJS.Dict<string | string[] | number>,
    cancellationToken: CancellationToken
  ): Promise<UpdateBlockMap> {
    const response = await this.fetchWithCancellation(url, headers, cancellationToken);
    if (!response.ok) throw new Error(`Blockmap request failed with HTTP ${response.status}: ${url}`);
    return this.parseBlockMap(Buffer.from(await response.arrayBuffer()), url.href);
  }

  private parseBlockMap(data: Buffer, source: string): UpdateBlockMap {
    try {
      return JSON.parse(gunzipSync(data).toString("utf8")) as UpdateBlockMap;
    } catch (error) {
      throw new Error(
        `Cannot parse blockmap "${source}": ${error instanceof Error ? error.message : String(error)}`
      );
    }
  }

  private async fetchWithCancellation(
    url: URL,
    headers: NodeJS.Dict<string | string[] | number>,
    cancellationToken: CancellationToken
  ): Promise<Response> {
    if (cancellationToken.cancelled) throw new CancellationError();
    const abortController = new AbortController();
    const cancel = () => abortController.abort();
    cancellationToken.once("cancel", cancel);
    try {
      return await this.netSession.fetch(url.href, {
        method: "GET",
        headers: requestHeadersForFetch(headers),
        signal: abortController.signal
      });
    } finally {
      cancellationToken.removeListener("cancel", cancel);
    }
  }
}
