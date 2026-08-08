export type UpdateCapturePressure = "healthy" | "elevated" | "critical";

export interface UpdateBlockMapFile {
  name: string;
  offset: number;
  checksums: string[];
  sizes: number[];
}

export interface UpdateBlockMap {
  version: "1" | "2";
  files: UpdateBlockMapFile[];
}

export type UpdateTransferOperation = {
  kind: "copy" | "download";
  start: number;
  end: number;
};

export interface UpdateTransferPlan {
  operations: UpdateTransferOperation[];
  copyBytes: number;
  downloadBytes: number;
  totalBytes: number;
}

export const maximumUpdateWriteBytes = 512 * 1024;

export function boundedUpdateWriteSize(remainingBytes: number): number {
  if (!Number.isFinite(remainingBytes) || remainingBytes <= 0) return 0;
  return Math.min(maximumUpdateWriteBytes, Math.floor(remainingBytes));
}

function checkedBlockMapFile(blockMap: UpdateBlockMap, label: string): UpdateBlockMapFile {
  if (blockMap.files.length !== 1) {
    throw new Error(`${label} blockmap must contain exactly one installer file`);
  }
  const file = blockMap.files[0];
  if (file.checksums.length !== file.sizes.length) {
    throw new Error(`${label} blockmap checksum and size counts differ`);
  }
  if (!file.sizes.every((size) => Number.isSafeInteger(size) && size > 0)) {
    throw new Error(`${label} blockmap contains an invalid block size`);
  }
  return file;
}

function appendOperation(
  operations: UpdateTransferOperation[],
  kind: UpdateTransferOperation["kind"],
  start: number,
  end: number
): void {
  const previous = operations.at(-1);
  if (previous?.kind === kind && previous.end === start) {
    previous.end = end;
    return;
  }
  operations.push({ kind, start, end });
}

export function buildUpdateTransferPlan(oldBlockMap: UpdateBlockMap, newBlockMap: UpdateBlockMap): UpdateTransferPlan {
  if (oldBlockMap.version !== newBlockMap.version) {
    throw new Error(`Blockmap versions differ (${oldBlockMap.version} and ${newBlockMap.version})`);
  }

  const oldFile = checkedBlockMapFile(oldBlockMap, "Old");
  const newFile = checkedBlockMapFile(newBlockMap, "New");
  if (oldFile.name !== newFile.name) {
    throw new Error(`Installer blockmap names differ (${oldFile.name} and ${newFile.name})`);
  }

  const oldBlocks = new Map<string, { offset: number; size: number }>();
  let oldOffset = oldFile.offset;
  for (let index = 0; index < oldFile.checksums.length; index += 1) {
    const checksum = oldFile.checksums[index];
    if (!oldBlocks.has(checksum)) {
      oldBlocks.set(checksum, { offset: oldOffset, size: oldFile.sizes[index] });
    }
    oldOffset += oldFile.sizes[index];
  }

  const operations: UpdateTransferOperation[] = [];
  let copyBytes = 0;
  let downloadBytes = 0;
  let newOffset = newFile.offset;
  for (let index = 0; index < newFile.checksums.length; index += 1) {
    const size = newFile.sizes[index];
    const oldBlock = oldBlocks.get(newFile.checksums[index]);
    if (oldBlock && oldBlock.size === size) {
      appendOperation(operations, "copy", oldBlock.offset, oldBlock.offset + size);
      copyBytes += size;
    } else {
      appendOperation(operations, "download", newOffset, newOffset + size);
      downloadBytes += size;
    }
    newOffset += size;
  }

  return {
    operations,
    copyBytes,
    downloadBytes,
    totalBytes: copyBytes + downloadBytes
  };
}

const mib = 1024 * 1024;

export interface UpdateWriteRateConfig {
  initialBytesPerSecond: number;
  minimumBytesPerSecond: number;
  maximumBytesPerSecond: number;
  adjustmentWindowBytes: number;
  targetUtilization: number;
  minimumMeasuredWriteMs: number;
  elevatedMaximumBytesPerSecond: number;
  criticalMaximumBytesPerSecond: number;
}

export const defaultUpdateWriteRateConfig: UpdateWriteRateConfig = {
  initialBytesPerSecond: 32 * mib,
  minimumBytesPerSecond: 4 * mib,
  maximumBytesPerSecond: 96 * mib,
  adjustmentWindowBytes: 16 * mib,
  targetUtilization: 0.65,
  minimumMeasuredWriteMs: 0.25,
  elevatedMaximumBytesPerSecond: 16 * mib,
  criticalMaximumBytesPerSecond: 4 * mib
};

export class UpdateWriteRateController {
  private currentRate: number;
  private observedServiceRate = 0;
  private healthyBytes = 0;
  private pressure: UpdateCapturePressure = "healthy";

  constructor(private readonly config: UpdateWriteRateConfig = defaultUpdateWriteRateConfig) {
    this.currentRate = this.clampRate(config.initialBytesPerSecond);
  }

  reset(): void {
    this.currentRate = this.clampRate(this.config.initialBytesPerSecond);
    this.observedServiceRate = 0;
    this.healthyBytes = 0;
    this.pressure = "healthy";
  }

  observePressure(nextPressure: UpdateCapturePressure): boolean {
    if (nextPressure === this.pressure) return false;
    const previousRate = this.currentRate;
    this.pressure = nextPressure;
    this.healthyBytes = 0;

    if (nextPressure === "critical") {
      this.currentRate = this.clampRate(Math.min(this.currentRate / 2, this.config.criticalMaximumBytesPerSecond));
    } else if (nextPressure === "elevated") {
      this.currentRate = this.clampRate(Math.min(
        this.currentRate - this.currentRate / 4,
        this.config.elevatedMaximumBytesPerSecond
      ));
    } else if (this.observedServiceRate > 0) {
      const target = this.clampRate(this.observedServiceRate * this.config.targetUtilization);
      this.currentRate = this.clampRate(Math.min(target, this.currentRate + 8 * mib));
    } else {
      this.currentRate = this.clampRate(Math.min(this.config.initialBytesPerSecond, this.currentRate + 8 * mib));
    }

    return this.currentRate !== previousRate;
  }

  observeWrite(bytes: number, durationMs: number): boolean {
    if (bytes <= 0) return false;
    if (durationMs >= this.config.minimumMeasuredWriteMs) {
      const sampleRate = Math.min(
        this.config.maximumBytesPerSecond,
        bytes * 1000 / durationMs
      );
      this.observedServiceRate = this.observedServiceRate === 0
        ? sampleRate
        : (this.observedServiceRate * 7 + sampleRate) / 8;
    }

    if (this.pressure !== "healthy") return false;
    this.healthyBytes += bytes;
    if (this.healthyBytes < this.config.adjustmentWindowBytes || this.observedServiceRate <= 0) return false;
    this.healthyBytes %= this.config.adjustmentWindowBytes;

    const previousRate = this.currentRate;
    const target = this.clampRate(this.observedServiceRate * this.config.targetUtilization);
    if (target > this.currentRate) {
      const increase = Math.max(this.currentRate / 8, 4 * mib);
      this.currentRate = this.clampRate(Math.min(target, this.currentRate + increase));
    } else if (target < this.currentRate) {
      this.currentRate = this.clampRate(Math.max(target, this.currentRate - this.currentRate / 4));
    }
    return this.currentRate !== previousRate;
  }

  currentBytesPerSecond(): number {
    return Math.round(this.currentRate);
  }

  observedBytesPerSecond(): number {
    return Math.round(this.observedServiceRate);
  }

  currentPressure(): UpdateCapturePressure {
    return this.pressure;
  }

  private clampRate(rate: number): number {
    return Math.max(
      this.config.minimumBytesPerSecond,
      Math.min(this.config.maximumBytesPerSecond, rate)
    );
  }
}

export class UpdateWritePacer {
  private nextWriteAtMs = 0;

  constructor(
    private readonly controller: UpdateWriteRateController,
    private readonly now: () => number = () => performance.now(),
    private readonly sleep: (delayMs: number) => Promise<void> =
      (delayMs) => new Promise<void>((resolve) => setTimeout(resolve, delayMs))
  ) {}

  reset(): void {
    this.nextWriteAtMs = 0;
  }

  async pace(bytes: number): Promise<number> {
    if (bytes <= 0) return 0;
    const nowMs = this.now();
    if (this.nextWriteAtMs < nowMs - 1000) this.nextWriteAtMs = nowMs;
    const waitMs = Math.max(0, this.nextWriteAtMs - nowMs);
    const writeBudgetMs = bytes * 1000 / this.controller.currentBytesPerSecond();
    this.nextWriteAtMs = Math.max(nowMs, this.nextWriteAtMs) + writeBudgetMs;
    if (waitMs > 0.5) await this.sleep(waitMs);
    return waitMs;
  }
}
