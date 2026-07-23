import { app, BrowserWindow, globalShortcut, ipcMain, shell, Tray, Menu, nativeImage, dialog, screen } from "electron";
import type { OpenDialogOptions } from "electron";
import { autoUpdater } from "electron-updater";
import { spawn, ChildProcessWithoutNullStreams } from "node:child_process";
import { createServer } from "node:http";
import { appendFileSync, copyFileSync, createReadStream, createWriteStream, existsSync, mkdirSync, readFileSync, readdirSync, renameSync, statSync, unlinkSync, writeFileSync } from "node:fs";
import { readdir as readdirAsync, stat as statAsync, rm as rmAsync } from "node:fs/promises";
import { format } from "node:util";
import { basename, dirname, extname, join, parse, normalize } from "node:path";
import { pathToFileURL } from "node:url";
import { createHash } from "node:crypto";
import type { ActiveProcess, AudioInputDevice, ClipRecord, ClipSettings, DisplayDevice, EngineDiagnostics, SaveClipResult, ClipSoundOption, UpdateState } from "../shared/types";

const defaultSettings: ClipSettings = {
  clipLengthSeconds: 30,
  fps: 30,
  bitrateMbps: 40,
  autoBitrate: false,
  maxAutoBitrateMbps: 80,
  nvencPreset: 3,
  resolutionPreset: "system",
  monitorMode: "primary",
  monitorId: "primary",
  startOnLogin: true,
  hotkey: "Ctrl+Shift+S",
  clipSound: "default.mp3",
  showNotification: true,
  notificationPosition: "top-right",
  saveFolder: "",
  importedVideoDirectories: [],
  importedVideoTitles: {},
  audioSources: [
    { id: "system", label: "System audio", kind: "system", enabled: true, omitIfSilent: true },
    {
      id: "mic",
      label: "Microphone",
      kind: "microphone",
      enabled: true,
      omitIfSilent: true,
      volume: 1.0,
      autoNoiseGate: true,
      noiseGateEnabled: true,
      noiseGateThreshold: 0.05,
      noiseGateDebounceMs: 180,
      micDeviceId: "",
      micDeviceMatchKey: "",
      micDeviceName: ""
    },
    { id: "game", label: "Detected game/app", kind: "game", enabled: false, omitIfSilent: true }
  ]
};

function clampNumber(value: unknown, fallback: number, min: number, max: number): number {
  const numeric = typeof value === "number" ? value : Number(value);
  if (!Number.isFinite(numeric)) return fallback;
  return Math.round(Math.min(max, Math.max(min, numeric)));
}

function saveClipEngineTimeoutMs(durationSeconds: number): number {
  const boundedDuration = clampNumber(durationSeconds, defaultSettings.clipLengthSeconds, 5, 600);
  return Math.min(30 * 60 * 1000, Math.max(3 * 60 * 1000, boundedDuration * 3000 + 60 * 1000));
}

function saveTimingNowMs(): number {
  return Date.now();
}

function saveTimingElapsedMs(startedAtMs: number): number {
  return Math.max(0, saveTimingNowMs() - startedAtMs);
}

function formatSaveTimingValue(value: unknown): string {
  if (typeof value === "string") return JSON.stringify(value);
  if (typeof value === "number" || typeof value === "boolean") return String(value);
  if (value === null || value === undefined) return String(value);
  return JSON.stringify(value);
}

let saveTimingLogStream: ReturnType<typeof createWriteStream> | null = null;

function appendSaveTimingLog(line: string): void {
  try {
    if (!saveTimingLogStream || saveTimingLogStream.destroyed) {
      const stream = createWriteStream(appDataPath("save-timing.log"), { flags: "a" });
      stream.on("error", () => {
        if (saveTimingLogStream === stream) saveTimingLogStream = null;
      });
      saveTimingLogStream = stream;
    }
    saveTimingLogStream.write(`[${new Date().toISOString()}] ${line}\n`);
  } catch {
    // Timing logs are diagnostic only; saving clips should never depend on file logging.
  }
}

function logSaveTimingLine(line: string): void {
  console.log(line);
  appendSaveTimingLog(line);
}

function logEngineStderr(chunk: Buffer): void {
  const text = chunk.toString();
  console.error(`[engine] ${text}`);
  for (const line of text.split(/\r?\n/)) {
    if (line.includes("[save-timing]")) appendSaveTimingLog(`[engine] ${line}`);
  }
}

function logSaveTiming(saveId: string, stage: string, startedAtMs: number, details: Record<string, unknown> = {}): void {
  const suffix = Object.entries(details)
    .filter(([, value]) => value !== undefined)
    .map(([key, value]) => `${key}=${formatSaveTimingValue(value)}`)
    .join(" ");
  logSaveTimingLine(`[save-timing] id=${saveId} stage=${stage} ms=${saveTimingElapsedMs(startedAtMs)}${suffix ? ` ${suffix}` : ""}`);
}

function normalizeSettings(settings: ClipSettings): ClipSettings {
  const defaultsById = new Map(defaultSettings.audioSources.map((source) => [source.id, source]));
  const existingById = new Map((settings.audioSources ?? []).map((source) => [source.id, source]));
  const audioSources = defaultSettings.audioSources.map((source) => {
    const existing = existingById.get(source.id);
    return { ...source, ...existing, enabled: existing?.enabled ?? source.enabled };
  });
  for (const source of settings.audioSources ?? []) {
    if (!defaultsById.has(source.id) && source.id !== "mix" && source.kind === "app") {
      audioSources.push({ ...source, enabled: source.enabled ?? false });
    }
  }
  const fps = [24, 30, 60].includes(Number(settings.fps)) ? Number(settings.fps) as ClipSettings["fps"] : defaultSettings.fps;
  const nvencPreset = [1, 2, 3, 4, 5].includes(Number(settings.nvencPreset)) ? Number(settings.nvencPreset) as ClipSettings["nvencPreset"] : defaultSettings.nvencPreset;
  const validResolutionPresets = new Set(["system", "144p", "360p", "720p", "1080p", "1440p", "4k"]);
  const resolutionPreset = validResolutionPresets.has(settings.resolutionPreset) ? settings.resolutionPreset : defaultSettings.resolutionPreset;
  const importedVideoDirectories = Array.from(new Set((settings.importedVideoDirectories ?? [])
    .filter((value): value is string => typeof value === "string" && value.trim().length > 0)
    .map((value) => normalize(value.trim()))));
  const importedVideoTitles = Object.fromEntries(Object.entries(settings.importedVideoTitles ?? {})
    .filter(([filePath, title]) => typeof filePath === "string" && filePath.trim() && typeof title === "string" && title.trim())
    .map(([filePath, title]) => [normalize(filePath.trim()), title.trim()]));
  return {
    ...defaultSettings,
    ...settings,
    clipLengthSeconds: clampNumber(settings.clipLengthSeconds, defaultSettings.clipLengthSeconds, 5, 600),
    fps,
    nvencPreset,
    bitrateMbps: clampNumber(settings.bitrateMbps, defaultSettings.bitrateMbps, 4, 120),
    autoBitrate: Boolean(settings.autoBitrate),
    maxAutoBitrateMbps: clampNumber(settings.maxAutoBitrateMbps, defaultSettings.maxAutoBitrateMbps, 4, 120),
    resolutionPreset,
    monitorId: typeof settings.monitorId === "string" && settings.monitorId.trim() ? settings.monitorId : defaultSettings.monitorId,
    importedVideoDirectories,
    importedVideoTitles,
    audioSources
  };
}

function resolutionSize(preset: ClipSettings["resolutionPreset"]): { width: number; height: number } {
  switch (preset) {
    case "144p": return { width: 256, height: 144 };
    case "360p": return { width: 640, height: 360 };
    case "720p": return { width: 1280, height: 720 };
    case "1080p": return { width: 1920, height: 1080 };
    case "1440p": return { width: 2560, height: 1440 };
    case "4k": return { width: 3840, height: 2160 };
    default: return { width: 0, height: 0 };
  }
}

function parseResolutionLabel(resolution?: string): { width: number; height: number } {
  const match = /^(\d+)x(\d+)$/i.exec(resolution || "");
  if (!match) return { width: 0, height: 0 };
  return { width: Number(match[1]), height: Number(match[2]) };
}

function recordingOutputResolution(settings: ClipSettings): { width: number; height: number } {
  return resolutionSize(settings.resolutionPreset);
}

function autoBitrateForResolution(resolution: { width: number; height: number }, fps: number, maxMbps: number): number {
  const width = resolution.width > 0 ? resolution.width : 1920;
  const height = resolution.height > 0 ? resolution.height : 1080;
  const pixels = width * height;
  const resolutionFactor = pixels / (1920 * 1080);
  const fpsFactor = Math.max(1, fps) / 30;
  const suggested = Math.round(24 * resolutionFactor * fpsFactor);
  return clampNumber(Math.min(suggested, maxMbps), 24, 4, 120);
}

async function bitrateResolution(settings: ClipSettings, targetResolution: { width: number; height: number }): Promise<{ width: number; height: number }> {
  if (targetResolution.width > 0 && targetResolution.height > 0) return targetResolution;

  const displays = await engine.listDisplayDevices();
  const selected = settings.monitorId && settings.monitorId !== "primary"
    ? displays.find((display) => display.id === settings.monitorId)
    : displays.find((display) => display.isPrimary);
  if (selected && selected.width > 0 && selected.height > 0) {
    return { width: selected.width, height: selected.height };
  }

  return { width: 1920, height: 1080 };
}

class EngineClient {
  private child: ChildProcessWithoutNullStreams | undefined;
  private nextId = 1;
  private buffer = "";
  private pending = new Map<number, { resolve: (value: unknown) => void; reject: (error: Error) => void; timer: ReturnType<typeof setTimeout> }>();
  private lastDiagnostics: EngineDiagnostics = {
    captureApi: "Windows.Graphics.Capture",
    activeEncoder: "Unavailable",
    encoderMode: "Unavailable",
    gpu: "Engine not running",
    microphoneDevice: "Unknown",
    display: "Primary display",
    hdrTonemapping: false,
    videoSourceResolution: "Unknown",
    videoOutputResolution: "Unknown",
    videoScaling: "Unknown",
    clipTargetResolution: "Unknown",
    codec: "H.264",
    resolution: "Native monitor",
    fps: 30,
    bitrateMbps: 40,
    hardwareAcceleration: false,
    droppedFrames: 0,
    nvencAvailable: false,
    engineRunning: false,
    d3d11Ready: false,
    captureReady: false,
    audioReady: false,
    muxReady: false,
    bufferedVideoPackets: 0,
    bufferedAudioPackets: 0,
    capturedFrames: 0,
    queuedFrames: 0,
    encoderAcceptedFrames: 0,
    encoderOutputPackets: 0,
    audioCapturedPackets: 0,
    bufferDurationSeconds: 0,
    degraded: true,
    status: "Native engine process has not started."
  };

  start(): void {
    if (this.child) return;
    const enginePath = resolveEnginePath();
    if (!enginePath) {
      this.lastDiagnostics.status = "clipture_engine.exe was not found. Run npm run build:engine.";
      return;
    }

    this.child = spawn(enginePath, [], { stdio: ["pipe", "pipe", "pipe"], windowsHide: true });
    this.child.stdout.on("data", (chunk: Buffer) => this.readStdout(chunk));
    this.child.stderr.on("data", (chunk: Buffer) => logEngineStderr(chunk));
    this.child.on("exit", () => {
      this.child = undefined;
      this.lastDiagnostics = { ...this.lastDiagnostics, activeEncoder: "Unavailable", encoderMode: "Unavailable", engineRunning: false, degraded: true, status: "Native engine exited." };
      for (const pending of this.pending.values()) {
        clearTimeout(pending.timer);
        pending.reject(new Error("Native engine exited."));
      }
      this.pending.clear();
    });
  }

  stop(): void {
    this.child?.kill();
    this.child = undefined;
  }

  async diagnostics(): Promise<EngineDiagnostics> {
    if (!this.child) this.start();
    if (!this.child) return this.lastDiagnostics;
    try {
      const diagnostics = await this.request<EngineDiagnostics>("getDiagnostics", {});
      this.lastDiagnostics = diagnostics;
      return diagnostics;
    } catch (error) {
      // The engine might be blocked (e.g. saving a clip), so just return the last known state instead of throwing.
      return this.lastDiagnostics;
    }
  }

  async listAudioInputDevices(): Promise<AudioInputDevice[]> {
    if (!this.child) this.start();
    if (!this.child) return [];
    try {
      return await this.request<AudioInputDevice[]>("listAudioInputDevices", {});
    } catch (error) {
      return [];
    }
  }

  async listDisplayDevices(): Promise<DisplayDevice[]> {
    if (!this.child) this.start();
    if (!this.child) return [];
    try {
      return await this.request<DisplayDevice[]>("listDisplayDevices", {});
    } catch (error) {
      return [];
    }
  }

  async saveClip(durationSeconds: number, saveFolder = readSettings().saveFolder): Promise<SaveClipResult> {
    if (!this.child) this.start();
    if (!this.child) {
      return { ok: false, message: this.lastDiagnostics.status };
    }
    return this.request<SaveClipResult>("saveClip", { durationSeconds, saveFolder }, saveClipEngineTimeoutMs(durationSeconds));
  }

  async configure(settings: ClipSettings): Promise<EngineDiagnostics> {
    if (!this.child) this.start();
    if (!this.child) return this.lastDiagnostics;
    const systemAudioSource = settings.audioSources.find((source) => source.id === "system");
    const targetResolution = recordingOutputResolution(settings);
    const effectiveBitrateMbps = settings.autoBitrate
      ? autoBitrateForResolution(await bitrateResolution(settings, targetResolution), settings.fps, settings.maxAutoBitrateMbps)
      : settings.bitrateMbps;
    const diagnostics = await this.request<EngineDiagnostics>("configure", {
      fps: settings.fps,
      bitrateMbps: effectiveBitrateMbps,
      nvencPreset: settings.nvencPreset,
      clipLengthSeconds: settings.clipLengthSeconds,
      monitorId: settings.monitorId,
      targetWidth: targetResolution.width,
      targetHeight: targetResolution.height,
      includeMixedAudio: false,
      includeSystemAudio: settings.audioSources.some((source) => source.id === "system" && source.enabled && (source.captureAllSystem ?? true)),
      includeMicrophoneAudio: settings.audioSources.some((source) => source.id === "mic" && source.enabled),
      captureGameAudio: settings.audioSources.some((source) => source.id === "game" && source.enabled),
      captureForegroundSystemAudio: Boolean(systemAudioSource?.enabled && !(systemAudioSource.captureAllSystem ?? true)),
      appAudioProcesses: Array.from(new Set(settings.audioSources
        .filter((source) => source.kind === "app" && source.enabled && source.processName?.trim())
        .map((source) => source.processName!.trim()))).join("|"),
      systemAudioProcesses: Array.from(new Set(settings.audioSources
        .filter((source) => source.id === "system" && source.enabled && !(source.captureAllSystem ?? true))
        .flatMap((source) => source.processNames || [])
        .map((name) => name.trim())
        .filter(Boolean))).join("|"),
      micVolume: settings.audioSources.find((source) => source.kind === "microphone")?.volume ?? 1.0,
      micIsolation: settings.audioSources.find((source) => source.kind === "microphone")?.voiceIsolation ?? false,
      micIsolationWeight: settings.audioSources.find((source) => source.kind === "microphone")?.voiceIsolationWeight ?? 1.0,
      noiseGateEnabled: settings.audioSources.find((source) => source.kind === "microphone")?.noiseGateEnabled ?? true,
      autoNoiseGate: settings.audioSources.find((source) => source.kind === "microphone")?.autoNoiseGate ?? true,
      noiseGateThreshold: settings.audioSources.find((source) => source.kind === "microphone")?.noiseGateThreshold ?? 0.05,
      noiseGateDebounceMs: settings.audioSources.find((source) => source.kind === "microphone")?.noiseGateDebounceMs ?? 180,
      micDeviceId: settings.audioSources.find((source) => source.kind === "microphone")?.micDeviceId ?? "",
      micDeviceMatchKey: settings.audioSources.find((source) => source.kind === "microphone")?.micDeviceMatchKey ?? "",
      micDeviceName: settings.audioSources.find((source) => source.kind === "microphone")?.micDeviceName ?? ""
    }, 15_000);
    this.lastDiagnostics = diagnostics;
    return diagnostics;
  }

  private request<T>(type: string, payload: Record<string, unknown>, timeoutMs: number = 5000): Promise<T> {
    if (!this.child) return Promise.reject(new Error("Native engine is not running."));
    const child = this.child;
    const id = this.nextId++;
    const message = JSON.stringify({ id, type, ...payload });
    return new Promise((resolve, reject) => {
      const fail = (error: Error) => {
        const pending = this.pending.get(id);
        if (!pending) return;
        this.pending.delete(id);
        clearTimeout(pending.timer);
        reject(error);
      };
      const timer = setTimeout(() => {
        if (!this.pending.has(id)) return;
        this.pending.delete(id);
        reject(new Error(`Engine request timed out: ${type}`));
      }, timeoutMs);
      this.pending.set(id, { resolve: resolve as (value: unknown) => void, reject, timer });
      try {
        child.stdin.write(`${message}\n`, (error) => {
          if (error) fail(error instanceof Error ? error : new Error(String(error)));
        });
      } catch (error) {
        fail(error instanceof Error ? error : new Error(String(error)));
      }
    });
  }

  async listRunningProcesses(includeExecutablePaths = false): Promise<ActiveProcess[]> {
    if (!this.child) this.start();
    if (!this.child) return [];
    try {
      return await this.request<ActiveProcess[]>("listRunningProcesses", { includeExecutablePaths }, 5000);
    } catch {
      return [];
    }
  }

  async processExecutablePath(processId: number): Promise<string> {
    if (!this.child) this.start();
    if (!this.child || !Number.isInteger(processId) || processId <= 0) return "";
    try {
      return await this.request<string>("getProcessExecutablePath", { processId }, 3000);
    } catch {
      return "";
    }
  }

  private readStdout(chunk: Buffer): void {
    this.buffer += chunk.toString("utf8");
    let newline = this.buffer.indexOf("\n");
    while (newline >= 0) {
      const line = this.buffer.slice(0, newline).trim();
      this.buffer = this.buffer.slice(newline + 1);
      if (line) this.handleLine(line);
      newline = this.buffer.indexOf("\n");
    }
  }

  private handleLine(line: string): void {
    try {
      const message = JSON.parse(line) as { id?: number; payload?: unknown; error?: string };
      if (typeof message.id !== "number") return;
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      clearTimeout(pending.timer);
      if (message.error) pending.reject(new Error(message.error));
      else pending.resolve(message.payload);
    } catch (error) {
      console.error("[engine] invalid json", line, error);
    }
  }
}

let mainWindow: BrowserWindow | undefined;
let notificationWindow: BrowserWindow | undefined;
let tray: Tray | undefined;
let isQuitting = false;
const engine = new EngineClient();
let currentHotkey = "";
const startHidden = process.argv.includes("--hidden") || process.argv.includes("--background");
const updateCheckIntervalMs = 30 * 60 * 1000;
let updateState: UpdateState = { status: "idle" };
let updateCheckTimer: ReturnType<typeof setInterval> | undefined;
let updateCheckInFlight = false;
let updateReady = false;
let updateListenersRegistered = false;
let saveClipInProgress = false;

app.setName("Clipture");
if (process.platform === "win32") {
  app.setAppUserModelId("app.clipture.desktop");
}

function setUpdateState(nextState: UpdateState): UpdateState {
  updateState = nextState;
  mainWindow?.webContents.send("updates:stateChanged", updateState);
  return updateState;
}

function updateVersion(version?: string): string | undefined {
  return version || updateState.version;
}

function installDownloadedUpdate(): void {
  if (!updateReady) return;
  isQuitting = true;
  autoUpdater.quitAndInstall();
}

async function performUpdateCheck(): Promise<UpdateState> {
  if (!app.isPackaged) {
    return setUpdateState({
      status: "idle",
      message: "Updates are checked in installed builds.",
      checkedAt: new Date().toISOString()
    });
  }

  if (updateReady || updateCheckInFlight) return updateState;

  updateCheckInFlight = true;
  try {
    await autoUpdater.checkForUpdates();
  } catch (error) {
    if (autoUpdater.logger) {
      autoUpdater.logger.error(`Error in checkForUpdates(): ${error instanceof Error ? error.stack || error.message : error}`);
    }
    setUpdateState({
      status: "error",
      version: updateState.version,
      message: error instanceof Error ? error.message : "Update check failed.",
      checkedAt: new Date().toISOString()
    });
  } finally {
    updateCheckInFlight = false;
  }
  return updateState;
}

function registerUpdateListeners(): void {
  if (updateListenersRegistered) return;
  updateListenersRegistered = true;

  autoUpdater.on("checking-for-update", () => {
    setUpdateState({
      status: "checking",
      version: updateState.version,
      message: "Checking for updates...",
      checkedAt: new Date().toISOString()
    });
  });
  autoUpdater.on("update-not-available", (updateInfo) => {
    setUpdateState({
      status: "idle",
      version: updateVersion(updateInfo.version),
      message: "Clipture is up to date.",
      checkedAt: new Date().toISOString()
    });
  });
  autoUpdater.on("update-available", (updateInfo) => {
    setUpdateState({
      status: "available",
      version: updateVersion(updateInfo.version),
      message: `Update ${updateInfo.version} is available.`,
      checkedAt: new Date().toISOString()
    });
  });
  autoUpdater.on("download-progress", (progress) => {
    const percent = Math.max(0, Math.min(100, Math.round(progress.percent)));
    setUpdateState({
      status: "downloading",
      version: updateState.version,
      message: `Downloading update ${percent}%`,
      checkedAt: updateState.checkedAt
    });
  });
  autoUpdater.on("error", (error) => {
    console.error("[updates]", error);
    if (autoUpdater.logger) {
      autoUpdater.logger.error(`Update event error: ${error instanceof Error ? error.stack || error.message : error}`);
    }
    setUpdateState({
      status: "error",
      version: updateState.version,
      message: error instanceof Error ? error.message : "Update check failed.",
      checkedAt: new Date().toISOString()
    });
  });
  autoUpdater.on("update-downloaded", async (updateInfo) => {
    updateReady = true;
    setUpdateState({
      status: "ready",
      version: updateVersion(updateInfo.version),
      message: `Clipture ${updateInfo.version} is ready to install.`,
      checkedAt: new Date().toISOString()
    });
  });
}

let updateLoggerConfigured = false;
function configureUpdateLogger() {
  if (updateLoggerConfigured) return;
  updateLoggerConfigured = true;
  
  const logFile = appDataPath("updates.log");
  const logToFile = (level: string, ...args: any[]) => {
    try {
      appendFileSync(logFile, `[${new Date().toISOString()}] [${level}] ${format(...args)}\n`);
    } catch (e) {}
  };
  
  logToFile("INFO", "Update logger initialized.");
  
  autoUpdater.logger = {
    info: (...args: any[]) => logToFile("INFO", ...args),
    warn: (...args: any[]) => logToFile("WARN", ...args),
    error: (...args: any[]) => logToFile("ERROR", ...args),
    debug: (...args: any[]) => logToFile("DEBUG", ...args),
  } as any;
}

function checkForAppUpdates(): void {
  autoUpdater.autoDownload = false;
  autoUpdater.disableDifferentialDownload = true;
  configureUpdateLogger();
  registerUpdateListeners();
  void performUpdateCheck();
  if (!updateCheckTimer) {
    updateCheckTimer = setInterval(() => void performUpdateCheck(), updateCheckIntervalMs);
  }
}

function appDataPath(file: string): string {
  const dir = join(app.getPath("userData"), "data");
  mkdirSync(dir, { recursive: true });
  return join(dir, file);
}

function settingsPath(): string {
  return appDataPath("settings.json");
}

function clipsPath(): string {
  return appDataPath("clips.json");
}

function soundsFolderPath(): string {
  const folder = appDataPath("sounds");
  mkdirSync(folder, { recursive: true });
  return folder;
}

function cleanupAbandonedPreviewCache(): void {
  setTimeout(() => {
    void rmAsync(appDataPath("preview-cache"), { recursive: true, force: true }).catch(() => {
      // Best effort cleanup for the removed disk-backed preview experiment.
    });
  }, 6000);
}

function safeSoundFileName(filePath: string): string {
  const parsed = parse(filePath);
  const base = (parsed.name || "sound")
    .replace(/[^a-z0-9._ -]/gi, "")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 80) || "sound";
  const ext = parsed.ext.toLowerCase();
  return `${base}${ext}`;
}

function uniqueSoundPath(filePath: string): string {
  const folder = soundsFolderPath();
  const safeName = safeSoundFileName(filePath);
  const parsed = parse(safeName);
  let candidate = join(folder, safeName);
  let index = 1;
  while (existsSync(candidate)) {
    candidate = join(folder, `${parsed.name}-${index}${parsed.ext}`);
    index += 1;
  }
  return candidate;
}

function labelFromSoundFileName(fileName: string): string {
  return basename(fileName, extname(fileName)).replace(/[-_]+/g, " ");
}

const bundledClipSounds = [
  { fileName: "default.mp3", label: "Default" },
  { fileName: "option2.wav", label: "Option 2" }
];

function bundledAssetPath(fileName: string): string | undefined {
  const candidates = [
    join(__dirname, "../../assets", fileName),
    join(process.cwd(), "assets", fileName),
    join(process.resourcesPath ?? "", "assets", fileName)
  ];
  return candidates.find((candidate) => existsSync(candidate));
}

function ensureBundledClipSounds(): void {
  const folder = soundsFolderPath();
  for (const sound of bundledClipSounds) {
    const destination = join(folder, sound.fileName);
    if (existsSync(destination)) continue;
    const source = bundledAssetPath(sound.fileName);
    if (source) copyFileSync(source, destination);
  }
}

function listClipSounds(): ClipSoundOption[] {
  ensureBundledClipSounds();
  const folder = soundsFolderPath();
  const builtInLabels = new Map(bundledClipSounds.map((sound) => [sound.fileName.toLowerCase(), sound.label]));
  return readdirSync(folder, { withFileTypes: true })
    .filter((entry) => entry.isFile() && /\.(mp3|wav|ogg)$/i.test(entry.name))
    .map((entry): ClipSoundOption => ({
      id: builtInLabels.has(entry.name.toLowerCase()) ? entry.name : `custom:${entry.name}`,
      label: builtInLabels.get(entry.name.toLowerCase()) ?? labelFromSoundFileName(entry.name),
      url: pathToFileURL(join(folder, entry.name)).toString(),
      builtIn: builtInLabels.has(entry.name.toLowerCase())
    }))
    .sort((a, b) => a.label.localeCompare(b.label));
}

let previewServerPort = 0;
const mixedAudioChunkSeconds = 8;

function playbackPayloadUrl(pathname: string, payload: unknown): string {
  const data = Buffer.from(JSON.stringify(payload)).toString("base64");
  return `http://127.0.0.1:${previewServerPort}${pathname}?data=${encodeURIComponent(data)}`;
}

function parsePlaybackPayload<T>(encoded: string | null): T | undefined {
  if (!encoded) return undefined;
  try {
    return JSON.parse(Buffer.from(encoded, "base64").toString("utf-8")) as T;
  } catch {
    return undefined;
  }
}

function mediaHeaders(extra: Record<string, string | number> = {}): Record<string, string | number> {
  return {
    "Access-Control-Allow-Origin": "*",
    "Accept-Ranges": "bytes",
    "Cache-Control": "no-store",
    "Content-Type": "video/mp4",
    ...extra
  };
}

function mediaContentType(filePath: string): string {
  switch (extname(filePath).toLowerCase()) {
    case ".webm": return "video/webm";
    case ".mov": return "video/quicktime";
    case ".mkv": return "video/x-matroska";
    case ".avi": return "video/x-msvideo";
    default: return "video/mp4";
  }
}

function audioChunkHeaders(extra: Record<string, string | number> = {}): Record<string, string | number> {
  return {
    "Access-Control-Allow-Origin": "*",
    "Cache-Control": "no-store",
    "Content-Type": "audio/wav",
    ...extra
  };
}

function sendMixedAudioChunk(
  req: import("node:http").IncomingMessage,
  res: import("node:http").ServerResponse,
  filePath: string,
  selectedAudioIndexes: number[],
  startSeconds: number,
  durationSeconds: number
): void {
  if (!existsSync(filePath)) {
    res.writeHead(404).end();
    return;
  }

  const audioIndexes = selectedAudioIndexes
    .map((index) => Math.trunc(Number(index)))
    .filter((index) => Number.isInteger(index) && index >= 0 && index < 32);

  const start = Math.max(0, Number.isFinite(startSeconds) ? startSeconds : 0);
  const duration = Math.min(20, Math.max(0.25, Number.isFinite(durationSeconds) ? durationSeconds : mixedAudioChunkSeconds));

  if (audioIndexes.length === 0) {
    res.writeHead(400).end();
    return;
  }

  if (req.method === "HEAD") {
    res.writeHead(200, audioChunkHeaders()).end();
    return;
  }

  const filterInputs = audioIndexes.map((index) => `[0:a:${index}]`).join("");
  const filter = audioIndexes.length === 1
    ? `${filterInputs}aresample=48000[aout]`
    : `${filterInputs}amix=inputs=${audioIndexes.length}:duration=longest:dropout_transition=0,aresample=48000[aout]`;

  const child = spawn(resolveFfmpegPath(), [
    "-hide_banner",
    "-loglevel",
    "error",
    "-ss",
    start.toFixed(3),
    "-t",
    duration.toFixed(3),
    "-i",
    filePath,
    "-filter_complex",
    filter,
    "-map",
    "[aout]",
    "-vn",
    "-sn",
    "-dn",
    "-ac",
    "2",
    "-ar",
    "48000",
    "-f",
    "wav",
    "pipe:1"
  ], { stdio: ["ignore", "pipe", "pipe"], windowsHide: true });

  const chunks: Buffer[] = [];
  let stderr = "";
  let settled = false;

  const endChunkResponse = (statusCode: number, body?: string | Buffer) => {
    if (settled || res.destroyed) return;
    settled = true;
    const length = Buffer.isBuffer(body) ? body.length : undefined;
    res.writeHead(statusCode, audioChunkHeaders(length !== undefined ? { "Content-Length": length } : {}));
    res.end(body);
  };

  child.stdout.on("data", (chunk: Buffer) => {
    chunks.push(chunk);
  });

  child.stderr.on("data", (chunk: Buffer) => {
    stderr += chunk.toString("utf8");
  });

  res.on("close", () => {
    if (settled) return;
    settled = true;
    child.kill();
  });

  child.on("error", (error) => {
    endChunkResponse(500, `ffmpeg unavailable: ${error.message}`);
  });

  child.on("exit", (code) => {
    if (settled || res.destroyed) return;
    if (code !== 0 || chunks.length === 0) {
      const message = stderr.trim() || `ffmpeg exited with code ${code}`;
      endChunkResponse(500, message);
      return;
    }

    const buffer = Buffer.concat(chunks);
    endChunkResponse(200, buffer);
  });
}

function sendFileRange(req: import("node:http").IncomingMessage, res: import("node:http").ServerResponse, filePath: string): void {
  if (!existsSync(filePath)) {
    res.writeHead(404).end();
    return;
  }

  const { size } = statSync(filePath);
  const headers = (extra: Record<string, string | number> = {}) => mediaHeaders({ "Content-Type": mediaContentType(filePath), ...extra });
  const range = req.headers.range;
  if (!range) {
    if (req.method === "HEAD") {
      res.writeHead(200, headers({ "Content-Length": size })).end();
      return;
    }
    res.writeHead(200, headers({ "Content-Length": size }));
    const stream = createReadStream(filePath);
    res.on("close", () => stream.destroy());
    stream.on("error", () => {
      if (!res.headersSent) res.writeHead(500);
      res.end();
    });
    stream.pipe(res);
    return;
  }

  const match = /^bytes=(\d*)-(\d*)$/.exec(range);
  if (!match) {
    res.writeHead(416, headers({ "Content-Range": `bytes */${size}` })).end();
    return;
  }

  const suffixLength = !match[1] && match[2] ? Number(match[2]) : 0;
  const requestedStart = suffixLength > 0 ? size - suffixLength : Number(match[1] || 0);
  const requestedEnd = suffixLength > 0 ? size - 1 : Number(match[2] || size - 1);
  const start = Math.min(Math.max(0, requestedStart), Math.max(0, size - 1));
  const end = Math.min(Math.max(start, requestedEnd), Math.max(0, size - 1));
  if (!Number.isFinite(start) || !Number.isFinite(end) || start >= size) {
    res.writeHead(416, headers({ "Content-Range": `bytes */${size}` })).end();
    return;
  }

  if (req.method === "HEAD") {
    res.writeHead(206, headers({
      "Content-Range": `bytes ${start}-${end}/${size}`,
      "Content-Length": end - start + 1
    })).end();
    return;
  }

  res.writeHead(206, headers({
    "Content-Range": `bytes ${start}-${end}/${size}`,
    "Content-Length": end - start + 1
  }));
  const stream = createReadStream(filePath, { start, end });
  res.on("close", () => stream.destroy());
  stream.on("error", () => {
    if (!res.headersSent) res.writeHead(500);
    res.end();
  });
  stream.pipe(res);
}

function setupPreviewServer() {
  const server = createServer((req, res) => {
    const url = new URL(req.url || "/", `http://${req.headers.host}`);
    if (url.pathname === "/clip") {
      const payload = parsePlaybackPayload<{ filePath?: string }>(url.searchParams.get("data"));
      if (!payload?.filePath) return res.writeHead(400).end();
      sendFileRange(req, res, payload.filePath);
      return;
    }

    if (url.pathname === "/audio-chunk") {
      const payload = parsePlaybackPayload<{ filePath?: string; selectedAudioIndexes?: number[] }>(url.searchParams.get("data"));
      const start = Number(url.searchParams.get("start") ?? 0);
      const duration = Number(url.searchParams.get("duration") ?? mixedAudioChunkSeconds);
      if (!payload?.filePath || !Array.isArray(payload.selectedAudioIndexes)) return res.writeHead(400).end();
      sendMixedAudioChunk(req, res, payload.filePath, payload.selectedAudioIndexes, start, duration);
      return;
    } else {
      res.writeHead(404).end();
    }
  });

  server.on("error", (e: NodeJS.ErrnoException) => {
    if (e.code === "EADDRINUSE") {
      server.listen(0, "127.0.0.1");
    }
  });

  server.on("listening", () => {
    previewServerPort = (server.address() as import("node:net").AddressInfo).port;
  });

  server.listen(4555, "127.0.0.1");
}

function mediaCachePath(filePath: string, folder: string, extension: string): string {
  const cacheDir = appDataPath(folder);
  mkdirSync(cacheDir, { recursive: true });
  const stats = statSync(filePath);
  const hash = createHash("sha1")
    .update(filePath)
    .update(String(stats.mtimeMs))
    .update(String(stats.size))
    .digest("hex");
  return join(cacheDir, `${hash}.${extension}`);
}

function resolveFfmpegPath(): string {
  const candidates = [
    process.env.FFMPEG_PATH,
    join(process.resourcesPath ?? "", "ffmpeg", "ffmpeg.exe"),
    join(process.cwd(), "ffmpeg", "ffmpeg.exe"),
    join(process.cwd(), "node_modules", "ffmpeg-static", "ffmpeg.exe")
  ].filter((candidate): candidate is string => Boolean(candidate));
  return candidates.find((candidate) => existsSync(candidate)) ?? "ffmpeg";
}

function runFfmpeg(args: string[]): Promise<{ ok: boolean; message: string }> {
  return new Promise((resolve) => {
    const child = spawn(resolveFfmpegPath(), args, { stdio: ["ignore", "ignore", "pipe"], windowsHide: true });
    let stderr = "";
    child.stderr?.on("data", (chunk: Buffer) => {
      stderr += chunk.toString("utf8");
    });
    child.on("error", (error) => {
      resolve({ ok: false, message: `ffmpeg is unavailable for mixed preview playback: ${error.message}` });
    });
    child.on("exit", (code) => {
      resolve({ ok: code === 0, message: code === 0 ? "Prepared mixed preview playback." : stderr.trim() || `ffmpeg exited with code ${code}.` });
    });
  });
}

let ffmpegFilterSupport: Promise<{ scaleCuda: boolean; scaleNpp: boolean }> | undefined;

function queryFfmpegFilterSupport(): Promise<{ scaleCuda: boolean; scaleNpp: boolean }> {
  if (!ffmpegFilterSupport) {
    ffmpegFilterSupport = new Promise((resolve) => {
      const child = spawn(resolveFfmpegPath(), ["-hide_banner", "-filters"], { stdio: ["ignore", "pipe", "pipe"], windowsHide: true });
      let output = "";
      child.stdout?.on("data", (chunk: Buffer) => {
        output += chunk.toString("utf8");
      });
      child.stderr?.on("data", (chunk: Buffer) => {
        output += chunk.toString("utf8");
      });
      child.on("error", () => resolve({ scaleCuda: false, scaleNpp: false }));
      child.on("exit", () => {
        resolve({
          scaleCuda: /\bscale_cuda\b/.test(output),
          scaleNpp: /\bscale_npp\b/.test(output)
        });
      });
    });
  }
  return ffmpegFilterSupport;
}

function parseCsvLine(line: string): string[] {
  const values: string[] = [];
  let current = "";
  let quoted = false;
  for (let index = 0; index < line.length; ++index) {
    const ch = line[index];
    if (ch === '"' && line[index + 1] === '"') {
      current += '"';
      ++index;
    } else if (ch === '"') {
      quoted = !quoted;
    } else if (ch === "," && !quoted) {
      values.push(current);
      current = "";
    } else {
      current += ch;
    }
  }
  values.push(current);
  return values;
}

type ProcessIconEntry = {
  name: string;
  pid: number;
  executablePath?: string;
};

type ClipIconCandidatePlan = {
  gameCandidates: string[];
  fallbackCandidates: string[];
};

class AsyncTaskLimiter {
  private active = 0;
  private readonly waiting: Array<() => void> = [];

  constructor(private readonly limit: number) {}

  run<T>(task: () => Promise<T>): Promise<T> {
    return new Promise<T>((resolve, reject) => {
      const start = () => {
        this.active++;
        void task().then(resolve, reject).finally(() => {
          this.active--;
          this.waiting.shift()?.();
        });
      };
      if (this.active < this.limit) start();
      else this.waiting.push(start);
    });
  }
}

const thumbnailTaskLimiter = new AsyncTaskLimiter(3);
const iconTaskLimiter = new AsyncTaskLimiter(2);
const thumbnailWidth = 480;
const thumbnailHeight = 270;
const thumbnailCacheLimit = 64;
const thumbnailDataUrlCache = new Map<string, { signature: string; dataUrl: string }>();
const pendingThumbnailTasks = new Map<string, Promise<string>>();

let iconProcessSnapshot: { expiresAt: number; promise: Promise<ProcessIconEntry[]> } | undefined;
const processIconDataUrls = new Map<string, string>();
const processExecutablePaths = new Map<number, string>();
const knownGameIconAliases = new Map<string, string[]>([
  ["counter-strike 2", ["cs2.exe"]],
  ["roblox", ["robloxplayerbeta.exe"]],
  ["fortnite", ["fortniteclient-win64-shipping.exe"]],
  ["valorant", ["valorant-win64-shipping.exe"]],
  ["league of legends", ["leagueclientuxrender.exe", "league of legends.exe"]],
  ["dead by daylight", ["deadbydaylight-win64-shipping.exe"]],
  ["minecraft", ["minecraft.windows.exe", "javaw.exe"]]
]);
let installedIconTargets: Array<{ name: string; root: string }> | undefined;
const installedExecutableIconPaths = new Map<string, string>();

function stripExecutableExtension(value: string): string {
  return value.replace(/\.exe$/i, "");
}

function lowerPath(value: string): string {
  return normalize(value).toLowerCase();
}

function readTextFileSafe(filePath: string): string {
  try {
    return readFileSync(filePath, "utf8");
  } catch {
    return "";
  }
}

function extractManifestField(text: string, field: string): string {
  const vdfPattern = new RegExp(`"${field}"\\s+"([^"]+)"`, "i");
  const vdfMatch = text.match(vdfPattern);
  if (vdfMatch?.[1]) return vdfMatch[1];
  const jsonPattern = new RegExp(`"${field}"\\s*:\\s*"([^"]+)"`, "i");
  const jsonMatch = text.match(jsonPattern);
  return jsonMatch?.[1] ?? "";
}

function steamLibraryRoots(): string[] {
  const programFilesX86 = process.env["ProgramFiles(x86)"];
  const steamRoot = programFilesX86 ? join(programFilesX86, "Steam") : "C:\\Program Files (x86)\\Steam";
  const roots = [steamRoot];
  const libraryFile = join(steamRoot, "steamapps", "libraryfolders.vdf");
  const text = readTextFileSafe(libraryFile);
  const pathPattern = /"path"\s+"([^"]+)"/gi;
  let match: RegExpExecArray | null;
  while ((match = pathPattern.exec(text))) {
    roots.push(match[1].replace(/\\\\/g, "\\"));
  }
  return Array.from(new Set(roots.map(lowerPath))).map((root) => normalize(root));
}

function scanInstalledIconTargets(): Array<{ name: string; root: string }> {
  if (installedIconTargets) return installedIconTargets;
  const targets: Array<{ name: string; root: string }> = [];

  for (const libraryRoot of steamLibraryRoots()) {
    const steamApps = join(libraryRoot, "steamapps");
    if (!existsSync(steamApps)) continue;
    let entries: string[] = [];
    try {
      entries = readdirSync(steamApps);
    } catch {
      entries = [];
    }
    for (const entry of entries) {
      if (!entry.toLowerCase().startsWith("appmanifest_") || !entry.toLowerCase().endsWith(".acf")) continue;
      const text = readTextFileSafe(join(steamApps, entry));
      const name = extractManifestField(text, "name");
      const installDir = extractManifestField(text, "installdir");
      if (name && installDir) targets.push({ name, root: join(steamApps, "common", installDir) });
    }
  }

  const programData = process.env.ProgramData || "C:\\ProgramData";
  const epicManifests = join(programData, "Epic", "EpicGamesLauncher", "Data", "Manifests");
  if (existsSync(epicManifests)) {
    let entries: string[] = [];
    try {
      entries = readdirSync(epicManifests);
    } catch {
      entries = [];
    }
    for (const entry of entries) {
      if (!entry.toLowerCase().endsWith(".item")) continue;
      const text = readTextFileSafe(join(epicManifests, entry));
      const name = extractManifestField(text, "DisplayName");
      const root = extractManifestField(text, "InstallLocation");
      if (name && root) targets.push({ name, root });
    }
  }

  installedIconTargets = targets;
  return targets;
}

function friendlyProcessNameForIcon(processName: string): string {
  const lower = basename(processName).toLowerCase();
  if (lower === "robloxplayerbeta.exe") return "Roblox";
  if (lower === "fortniteclient-win64-shipping.exe") return "Fortnite";
  if (lower === "valorant-win64-shipping.exe") return "VALORANT";
  if (lower === "cs2.exe") return "Counter-Strike 2";
  if (lower === "leagueclientuxrender.exe" || lower === "league of legends.exe") return "League of Legends";
  if (lower === "deadbydaylight-win64-shipping.exe") return "Dead by Daylight";
  if (lower === "minecraft.windows.exe" || lower === "javaw.exe") return "Minecraft";
  return stripExecutableExtension(basename(processName));
}

function isIgnoredIconCandidate(value: string): boolean {
  return ["", "foreground app", "clipture", "system audio", "microphone", "mixed preview"].includes(value.toLowerCase());
}

function isKnownGameIconCandidate(value: string): boolean {
  const lower = stripExecutableExtension(value.trim().toLowerCase());
  if (!lower) return false;
  if (knownGameIconAliases.has(lower)) return true;
  for (const [gameName, aliases] of knownGameIconAliases) {
    if (gameName === lower) return true;
    if (aliases.some((alias) => stripExecutableExtension(alias.toLowerCase()) === lower)) return true;
  }
  return false;
}

function uniqueIconCandidates(values: string[]): string[] {
  const seen = new Set<string>();
  return values
    .map((value) => value.trim())
    .filter((value) => {
      const key = value.toLowerCase();
      if (isIgnoredIconCandidate(value) || seen.has(key)) return false;
      seen.add(key);
      return true;
    });
}

function clipIconCandidatePlan(clip: ClipRecord, preferredLabels: string[] = []): ClipIconCandidatePlan {
  const hasPreferredLabels = preferredLabels.some((label) => label.trim().length > 0);
  const preferredCandidates = uniqueIconCandidates(preferredLabels);
  if (hasPreferredLabels) {
    return {
      gameCandidates: preferredCandidates.filter(isKnownGameIconCandidate),
      fallbackCandidates: preferredCandidates
    };
  }

  const trackCandidates = clip.audioTracks
    .filter((track) => !["system-loopback-pcm", "microphone-pcm", "mixed-preview-pcm"].includes(track))
    .map((track) => track.replace(/^(app|game):/i, ""));
  const gameTrackCandidates = clip.audioTracks
    .filter((track) => track.startsWith("game:"))
    .map((track) => track.slice(5));
  const values = [
    clip.gameOrApp,
    ...(clip.focusedApps ?? []),
    ...trackCandidates
  ];
  const explicitGameCandidates = [
    ...gameTrackCandidates,
    ...(clip.isGame ? [clip.gameOrApp] : [])
  ];
  const knownGameCandidates = values.filter(isKnownGameIconCandidate);

  const gameCandidates = uniqueIconCandidates([
    ...explicitGameCandidates,
    ...knownGameCandidates
  ]);
  const fallbackCandidates = uniqueIconCandidates([
    ...gameCandidates,
    ...values
  ]);

  return {
    gameCandidates,
    fallbackCandidates
  };
}

function processIconSearchTokens(candidates: string[]): Set<string> {
  const tokens = new Set<string>();
  for (const candidate of candidates) {
    const lower = candidate.toLowerCase();
    const base = stripExecutableExtension(lower);
    tokens.add(lower);
    tokens.add(base);
    if (!lower.endsWith(".exe")) tokens.add(`${lower}.exe`);
    for (const alias of knownGameIconAliases.get(base) ?? []) {
      tokens.add(alias);
      tokens.add(stripExecutableExtension(alias));
    }
  }
  return tokens;
}

function candidateAliasNames(candidates: string[]): string[] {
  const aliases = new Set<string>();
  for (const candidate of candidates) {
    const lower = candidate.toLowerCase();
    const base = stripExecutableExtension(lower);
    aliases.add(lower);
    aliases.add(base);
    if (!lower.endsWith(".exe")) aliases.add(`${lower}.exe`);
    for (const alias of knownGameIconAliases.get(base) ?? []) {
      aliases.add(alias.toLowerCase());
      aliases.add(stripExecutableExtension(alias.toLowerCase()));
    }
  }
  return [...aliases].filter(Boolean);
}

function findExecutableInDirectory(root: string, aliases: string[]): string {
  const wanted = new Set(aliases.map((alias) => alias.toLowerCase()).flatMap((alias) => [alias, `${stripExecutableExtension(alias)}.exe`]));
  const queue = [root];
  let scanned = 0;
  const maxEntries = 2500;
  while (queue.length > 0 && scanned < maxEntries) {
    const current = queue.shift()!;
    let entries: string[] = [];
    try {
      entries = readdirSync(current);
    } catch {
      continue;
    }
    for (const entry of entries) {
      if (++scanned > maxEntries) break;
      const fullPath = join(current, entry);
      let stats;
      try {
        stats = statSync(fullPath);
      } catch {
        continue;
      }
      if (stats.isDirectory()) {
        const lower = entry.toLowerCase();
        if (!["bin", "binaries", "game", "win64", "windows", "retail", "csgo"].includes(lower) && current !== root) continue;
        queue.push(fullPath);
      } else if (entry.toLowerCase().endsWith(".exe") && wanted.has(entry.toLowerCase())) {
        return fullPath;
      }
    }
  }
  return "";
}

function listProcessesForIcons(): Promise<ProcessIconEntry[]> {
  const now = Date.now();
  if (iconProcessSnapshot && iconProcessSnapshot.expiresAt > now) return iconProcessSnapshot.promise;

  const promise = engine.listRunningProcesses(false).then((processes) => processes.map((process) => ({
    name: process.name,
    pid: process.pid,
    executablePath: process.executablePath
  }))).catch(() => []);

  iconProcessSnapshot = { expiresAt: now + 30_000, promise };
  return promise;
}

async function executablePathForProcess(process: ProcessIconEntry | undefined): Promise<string> {
  if (!process) return "";
  if (process.executablePath) return process.executablePath;
  if (processExecutablePaths.has(process.pid)) return processExecutablePaths.get(process.pid) ?? "";
  const executablePath = await engine.processExecutablePath(process.pid);
  processExecutablePaths.set(process.pid, executablePath);
  return executablePath;
}

function findIconProcess(candidates: string[], processes: ProcessIconEntry[]): ProcessIconEntry | undefined {
  const tokens = processIconSearchTokens(candidates);
  for (const candidate of candidates) {
    const candidateTokens = processIconSearchTokens([candidate]);
    const match = processes.find((process) => {
      const name = basename(process.name).toLowerCase();
      const base = stripExecutableExtension(name);
      const friendly = friendlyProcessNameForIcon(name).toLowerCase();
      return candidateTokens.has(name) || candidateTokens.has(base) || candidateTokens.has(friendly);
    });
    if (match) return match;
  }
  return processes.find((process) => {
    const name = basename(process.name).toLowerCase();
    const base = stripExecutableExtension(name);
    const friendly = friendlyProcessNameForIcon(name).toLowerCase();
    return tokens.has(name) || tokens.has(base) || tokens.has(friendly);
  });
}

function findInstalledExecutableForIcon(candidates: string[]): string {
  if (candidates.length === 0) return "";
  const aliases = candidateAliasNames(candidates);
  const cacheKey = [...aliases].sort().join("|");
  if (installedExecutableIconPaths.has(cacheKey)) return installedExecutableIconPaths.get(cacheKey) ?? "";
  const candidateNames = new Set(aliases.map((alias) => stripExecutableExtension(alias).toLowerCase()));
  for (const target of scanInstalledIconTargets()) {
    const targetName = stripExecutableExtension(target.name).toLowerCase();
    const matchingGameName = candidateNames.has(targetName) || aliases.some((alias) => stripExecutableExtension(alias).toLowerCase() === targetName);
    if (!matchingGameName || !existsSync(target.root)) continue;
    const executablePath = findExecutableInDirectory(target.root, aliases);
    if (executablePath) {
      installedExecutableIconPaths.set(cacheKey, executablePath);
      return executablePath;
    }
  }
  installedExecutableIconPaths.set(cacheKey, "");
  return "";
}

async function clipIconUrl(clip: ClipRecord, preferredLabels: string[] = []): Promise<string> {
  return iconTaskLimiter.run(async () => {
    const candidatePlan = clipIconCandidatePlan(clip, preferredLabels);
    const processes = await listProcessesForIcons();
    const gameProcess = findIconProcess(candidatePlan.gameCandidates, processes);
    const process = gameProcess ?? (
      candidatePlan.gameCandidates.length > 0
        ? undefined
        : findIconProcess(candidatePlan.fallbackCandidates, processes)
    );
    const executablePath =
      await executablePathForProcess(process) ||
      findInstalledExecutableForIcon(candidatePlan.gameCandidates) ||
      findInstalledExecutableForIcon(candidatePlan.fallbackCandidates);
    return executableIconDataUrl(executablePath, 20);
  });
}

async function executableIconDataUrl(executablePath: string | undefined, size: number): Promise<string> {
  if (!executablePath || !existsSync(executablePath)) return "";

  const cacheKey = `${executablePath.toLowerCase()}|${size}`;
  const cached = processIconDataUrls.get(cacheKey);
  if (cached) return cached;

  try {
    const iconSize: "small" | "normal" = size <= 20 ? "small" : "normal";
    const icon = await app.getFileIcon(executablePath, { size: iconSize });
    if (icon.isEmpty()) return "";
    const dataUrl = icon.resize({ width: size, height: size }).toDataURL();
    processIconDataUrls.set(cacheKey, dataUrl);
    return dataUrl;
  } catch {
    return "";
  }
}

async function processIconUrl(processName: string, executablePath?: string): Promise<string> {
  const pathIcon = await executableIconDataUrl(executablePath, 36);
  if (pathIcon) return pathIcon;

  const candidates = uniqueIconCandidates([processName]);
  if (candidates.length === 0) return "";
  const process = findIconProcess(candidates, await listProcessesForIcons());
  const resolvedPath = await executablePathForProcess(process) || findInstalledExecutableForIcon(candidates);
  return executableIconDataUrl(resolvedPath, 36);
}

async function listActiveProcesses(): Promise<ActiveProcess[]> {
  const iconEntries = await engine.listRunningProcesses(true);
  if (iconEntries.length > 0) {
    const byName = new Map<string, ActiveProcess>();
    for (const process of iconEntries) {
      const key = process.name.toLowerCase();
      const existing = byName.get(key);
      if (!existing || (!existing.executablePath && process.executablePath)) {
        byName.set(key, {
          name: process.name,
          pid: process.pid,
          executablePath: process.executablePath
        });
      }
    }
    return [...byName.values()].sort((a, b) => a.name.localeCompare(b.name));
  }

  return new Promise((resolve) => {
    const child = spawn("tasklist.exe", ["/fo", "csv", "/nh"], { windowsHide: true });
    let stdout = "";
    child.stdout.on("data", (chunk: Buffer) => {
      stdout += chunk.toString("utf8");
    });
    child.on("error", () => resolve([]));
    child.on("exit", () => {
      const byName = new Map<string, ActiveProcess>();
      for (const line of stdout.split(/\r?\n/)) {
        if (!line.trim()) continue;
        const [name, pid] = parseCsvLine(line);
        const numericPid = Number(pid);
        if (!name || !Number.isFinite(numericPid)) continue;
        const key = name.toLowerCase();
        if (!byName.has(key)) byName.set(key, { name, pid: numericPid });
      }
      resolve([...byName.values()].sort((a, b) => a.name.localeCompare(b.name)));
    });
  });
}

async function processClipFile(
  filePath: string,
  target: { width: number; height: number },
  actual: { width: number; height: number },
  reencodeBitrateMbps: number,
  audioTracks: string[],
  saveTimingId?: string
): Promise<{ ok: boolean; message: string; tracksUpdated: boolean; newTracks: string[] }> {
  const totalStartedAt = saveTimingNowMs();
  const settings = readSettings();
  const systemSource = settings.audioSources.find((s) => s.id === "system" && s.enabled);
  const captureAllSystem = systemSource?.captureAllSystem ?? true;

  const separateAppSources = new Set(
    settings.audioSources
      .filter((s) => s.kind === "app" && s.enabled && s.processName)
      .map((s) => `app:${s.processName}`)
  );

  const systemMixTracks: number[] = [];
  const separateTracks: number[] = [];
  let microphoneTrack = -1;

  for (let i = 0; i < audioTracks.length; i++) {
    const track = audioTracks[i];
    if (track === "microphone-pcm") {
      microphoneTrack = i;
    } else if (track === "system-loopback-pcm" || (track.startsWith("app:") && !separateAppSources.has(track))) {
      systemMixTracks.push(i);
    } else {
      separateTracks.push(i);
    }
  }

  const needsScale =
    target.width > 0 &&
    target.height > 0 &&
    (actual.width !== target.width || actual.height !== target.height);
  const needsMix = systemMixTracks.length > 1;

  if (!existsSync(filePath)) {
    if (saveTimingId) {
      logSaveTiming(saveTimingId, "postprocess.missing_file", totalStartedAt, { filePath });
    }
    return { ok: true, message: "Clip file was not found.", tracksUpdated: false, newTracks: audioTracks };
  }

  const outputPath = filePath.replace(/\.mp4$/i, `.processed.mp4`);
  
  const args = [
    "-y",
    "-hide_banner",
    "-loglevel", "error",
    "-i", filePath
  ];

  if (needsMix) {
    const inputs = systemMixTracks.map((idx) => `[0:a:${idx}]`).join("");
    args.push("-filter_complex", `${inputs}amix=inputs=${systemMixTracks.length}:duration=longest:dropout_transition=0[sys_mix]`);
  }

  args.push("-map", "0:v:0");

  const newTracks: string[] = [];

  if (needsMix) {
    args.push("-map", "[sys_mix]");
    newTracks.push("System audio");
  } else if (systemMixTracks.length === 1) {
    args.push("-map", `0:a:${systemMixTracks[0]}`);
    newTracks.push("System audio");
  }

  if (microphoneTrack !== -1) {
    args.push("-map", `0:a:${microphoneTrack}`);
    newTracks.push("microphone-pcm");
  }

  for (const idx of separateTracks) {
    args.push("-map", `0:a:${idx}`);
    const rawTrack = audioTracks[idx];
    if (rawTrack === "system-loopback-pcm") {
      newTracks.push("System audio");
    } else if (rawTrack === "microphone-pcm") {
      newTracks.push("Microphone");
    } else if (rawTrack.startsWith("app:")) {
      newTracks.push(rawTrack.substring(4).replace(/\.exe$/i, ""));
    } else if (rawTrack.startsWith("game:")) {
      newTracks.push(rawTrack.substring(5).replace(/\.exe$/i, ""));
    } else {
      newTracks.push(rawTrack);
    }
  }

  const tracksChanged = newTracks.length !== audioTracks.length || newTracks.some((t, i) => t !== audioTracks[i]);

  if (!needsScale && !needsMix) {
    if (saveTimingId) {
      logSaveTiming(saveTimingId, "postprocess.skipped", totalStartedAt, {
        needsScale,
        needsMix,
        tracksChanged,
        audioTracks: audioTracks.length,
        target: `${target.width}x${target.height}`,
        actual: `${actual.width}x${actual.height}`
      });
    }
    return { ok: true, message: "No processing needed.", tracksUpdated: tracksChanged, newTracks };
  }

  if (needsScale) {
    const scaleFilter = `scale=${target.width}:${target.height}:force_original_aspect_ratio=decrease,pad=${target.width}:${target.height}:(ow-iw)/2:(oh-ih)/2`;
    args.push("-vf", scaleFilter, "-c:v", "h264_nvenc", "-preset", "p5", "-b:v", `${clampNumber(reencodeBitrateMbps, 20, 4, 120)}M`);
  } else {
    args.push("-c:v", "copy");
  }

  if (needsMix) {
    args.push("-c:a", "aac", "-b:a", "192k");
  } else {
    args.push("-c:a", "copy");
  }

  args.push("-movflags", "+faststart", outputPath);

  if (saveTimingId) {
    logSaveTiming(saveTimingId, "postprocess.start", totalStartedAt, {
      needsScale,
      needsMix,
      tracksChanged,
      video: needsScale ? "h264_nvenc" : "copy",
      audio: needsMix ? "aac" : "copy",
      audioTracks: audioTracks.length,
      target: `${target.width}x${target.height}`,
      actual: `${actual.width}x${actual.height}`
    });
  }
  const ffmpegStartedAt = saveTimingNowMs();
  const ffmpeg = await runFfmpeg(args);
  if (saveTimingId) {
    logSaveTiming(saveTimingId, "postprocess.ffmpeg", ffmpegStartedAt, { ok: ffmpeg.ok });
  }
  if (!ffmpeg.ok || !existsSync(outputPath)) {
    if (saveTimingId) {
      logSaveTiming(saveTimingId, "postprocess.total", totalStartedAt, { ok: false, reason: ffmpeg.message });
    }
    return { ok: ffmpeg.ok, message: ffmpeg.message, tracksUpdated: false, newTracks: audioTracks };
  }

  try {
    unlinkSync(filePath);
    renameSync(outputPath, filePath);
  } catch (error) {
    if (saveTimingId) {
      logSaveTiming(saveTimingId, "postprocess.total", totalStartedAt, { ok: false, reason: "replace_failed" });
    }
    return { ok: false, message: error instanceof Error ? error.message : "Could not replace clip with processed output.", tracksUpdated: false, newTracks: audioTracks };
  }

  if (saveTimingId) {
    logSaveTiming(saveTimingId, "postprocess.total", totalStartedAt, { ok: true });
  }
  return { ok: true, message: needsScale && needsMix ? "Scaled and mixed audio." : needsScale ? "Scaled video." : "Mixed system audio.", tracksUpdated: needsMix, newTracks };
}

async function stitchSegmentedClip(
  clip: ClipRecord,
  target: { width: number; height: number },
  reencodeBitrateMbps: number,
  saveTimingId?: string
): Promise<{ ok: boolean; message: string }> {
  const totalStartedAt = saveTimingNowMs();
  const segmentFiles = clip.segmentFiles?.filter((file) => existsSync(file)) ?? [];
  if (segmentFiles.length <= 1) {
    if (saveTimingId) logSaveTiming(saveTimingId, "stitch.skipped", totalStartedAt, { segmentCount: segmentFiles.length });
    return { ok: true, message: "No segment stitching needed." };
  }

  const targetWidth = target.width > 0 ? target.width : parseResolutionLabel(clip.recommendedResolution).width;
  const targetHeight = target.height > 0 ? target.height : parseResolutionLabel(clip.recommendedResolution).height;
  if (targetWidth <= 0 || targetHeight <= 0) {
    if (saveTimingId) logSaveTiming(saveTimingId, "stitch.total", totalStartedAt, { ok: false, reason: "missing_target_resolution" });
    return { ok: false, message: "Segment stitching needs a target resolution." };
  }

  const segmentResolutions = clip.segmentResolutions?.map(parseResolutionLabel) ?? [];
  const sameSegmentResolution =
    segmentResolutions.length === segmentFiles.length &&
    segmentResolutions.length > 0 &&
    segmentResolutions.every((resolution) =>
      resolution.width === segmentResolutions[0].width &&
      resolution.height === segmentResolutions[0].height
    );
  const targetMatchesSegments =
    sameSegmentResolution &&
    segmentResolutions[0].width === targetWidth &&
    segmentResolutions[0].height === targetHeight;
  if (saveTimingId) {
    logSaveTiming(saveTimingId, "stitch.start", totalStartedAt, {
      segmentCount: segmentFiles.length,
      target: `${targetWidth}x${targetHeight}`,
      sameResolution: sameSegmentResolution,
      canStreamCopy: targetMatchesSegments,
      audioTracks: clip.audioTracks.length
    });
  }

  if (targetMatchesSegments) {
    const concatListPath = join(dirname(clip.filePath), `${parse(clip.filePath).name}.concat.txt`);
    const concatList = segmentFiles
      .map((file) => `file '${file.replace(/'/g, "'\\''")}'`)
      .join("\n");
    try {
      writeFileSync(concatListPath, concatList);
      const copyStartedAt = saveTimingNowMs();
      const ffmpeg = await runFfmpeg([
        "-y",
        "-hide_banner",
        "-loglevel", "error",
        "-f", "concat",
        "-safe", "0",
        "-i", concatListPath,
        "-c", "copy",
        "-movflags", "+faststart",
        clip.filePath
      ]);
      if (saveTimingId) {
        logSaveTiming(saveTimingId, "stitch.ffmpeg", copyStartedAt, { ok: ffmpeg.ok, mode: "copy" });
      }
      try {
        unlinkSync(concatListPath);
      } catch {
        // Best effort cleanup.
      }
      if (ffmpeg.ok && existsSync(clip.filePath)) {
        for (const file of segmentFiles) {
          try {
            unlinkSync(file);
          } catch {
            // Best effort cleanup; the stitched clip is already written.
          }
        }
        clip.segmentFiles = undefined;
        clip.segmentResolutions = undefined;
        clip.resolution = `${targetWidth}x${targetHeight}`;
        if (saveTimingId) logSaveTiming(saveTimingId, "stitch.total", totalStartedAt, { ok: true, mode: "copy" });
        return { ok: true, message: "Stitched segmented video with stream copy." };
      }
      try {
        if (existsSync(clip.filePath)) unlinkSync(clip.filePath);
      } catch {
        // Fall back to re-encoding below.
      }
    } catch (error) {
      if (saveTimingId) {
        logSaveTiming(saveTimingId, "stitch.copy_fallback", totalStartedAt, {
          reason: error instanceof Error ? error.message : "concat_list_failed"
        });
      }
    }
  }

  const canUseGpuScale =
    segmentResolutions.length === segmentFiles.length &&
    segmentResolutions.every((resolution) =>
      resolution.width > 0 &&
      resolution.height > 0 &&
      resolution.width * targetHeight === resolution.height * targetWidth
  );
  const filterStartedAt = saveTimingNowMs();
  const filterSupport = canUseGpuScale ? await queryFfmpegFilterSupport() : { scaleCuda: false, scaleNpp: false };
  if (saveTimingId) {
    logSaveTiming(saveTimingId, "stitch.filter_support", filterStartedAt, {
      canUseGpuScale,
      scaleCuda: filterSupport.scaleCuda,
      scaleNpp: filterSupport.scaleNpp
    });
  }

  const buildArgs = (mode: "cuda" | "npp" | "cpu"): string[] => {
    const args = ["-y", "-hide_banner", "-loglevel", "error"];
    for (const file of segmentFiles) {
      args.push("-i", file);
    }

    const filters: string[] = [];
    for (let i = 0; i < segmentFiles.length; i++) {
      if (mode === "cuda") {
        filters.push(`[${i}:v:0]format=nv12,hwupload_cuda,scale_cuda=${targetWidth}:${targetHeight},setsar=1[v${i}]`);
      } else if (mode === "npp") {
        filters.push(`[${i}:v:0]format=nv12,hwupload_cuda,scale_npp=${targetWidth}:${targetHeight},setsar=1[v${i}]`);
      } else {
        filters.push(
          `[${i}:v:0]scale=${targetWidth}:${targetHeight}:force_original_aspect_ratio=decrease,` +
          `pad=${targetWidth}:${targetHeight}:(ow-iw)/2:(oh-ih)/2,setsar=1[v${i}]`
        );
      }
    }
    filters.push(`${segmentFiles.map((_, i) => `[v${i}]`).join("")}concat=n=${segmentFiles.length}:v=1:a=0[vout]`);

    const audioLabels: string[] = [];
    for (let trackIndex = 0; trackIndex < clip.audioTracks.length; trackIndex++) {
      const label = `a${trackIndex}`;
      filters.push(
        `${segmentFiles.map((_, segmentIndex) => `[${segmentIndex}:a:${trackIndex}]`).join("")}` +
        `concat=n=${segmentFiles.length}:v=0:a=1[${label}]`
      );
      audioLabels.push(label);
    }

    args.push("-filter_complex", filters.join(";"), "-map", "[vout]");
    for (const label of audioLabels) {
      args.push("-map", `[${label}]`);
    }
    args.push(
      "-c:v", "h264_nvenc",
      "-preset", "p5",
      "-b:v", `${clampNumber(reencodeBitrateMbps, 20, 4, 120)}M`,
      "-c:a", "aac",
      "-b:a", "192k",
      "-movflags", "+faststart",
      clip.filePath
    );
    return args;
  };

  const modes: Array<"cuda" | "npp" | "cpu"> = [];
  if (filterSupport.scaleCuda) modes.push("cuda");
  if (filterSupport.scaleNpp) modes.push("npp");
  modes.push("cpu");

  let ffmpeg = { ok: false, message: "ffmpeg did not run." };
  let usedMode: "cuda" | "npp" | "cpu" = "cpu";
  for (const mode of modes) {
    usedMode = mode;
    const ffmpegStartedAt = saveTimingNowMs();
    ffmpeg = await runFfmpeg(buildArgs(mode));
    if (saveTimingId) {
      logSaveTiming(saveTimingId, "stitch.ffmpeg", ffmpegStartedAt, { ok: ffmpeg.ok, mode });
    }
    if (ffmpeg.ok && existsSync(clip.filePath)) break;
    try {
      if (existsSync(clip.filePath)) unlinkSync(clip.filePath);
    } catch {
      // Retry cleanup is best effort.
    }
  }
  if (!ffmpeg.ok || !existsSync(clip.filePath)) {
    if (saveTimingId) logSaveTiming(saveTimingId, "stitch.total", totalStartedAt, { ok: false, mode: usedMode, reason: ffmpeg.message });
    return { ok: ffmpeg.ok, message: ffmpeg.message };
  }

  for (const file of segmentFiles) {
    try {
      unlinkSync(file);
    } catch {
      // Best effort cleanup; the stitched clip is already written.
    }
  }
  clip.segmentFiles = undefined;
  clip.segmentResolutions = undefined;
  clip.resolution = `${targetWidth}x${targetHeight}`;
  if (saveTimingId) logSaveTiming(saveTimingId, "stitch.total", totalStartedAt, { ok: true, mode: usedMode });
  return { ok: true, message: usedMode === "cpu" ? "Stitched segmented video." : `Stitched segmented video with GPU ${usedMode} scaling.` };
}

async function clipPlaybackUrl(filePath: string, audioTracks: string[]): Promise<{ url: string; mixed: boolean; message: string; audioChunkUrl?: string; audioChunkSeconds?: number }> {
  if (!existsSync(filePath)) return { url: "", mixed: false, message: "Clip file was not found." };

  const selectedAudioIndexes = audioTracks
    .map((track, index) => ({ track, index }))
    .filter(({ track }) => track !== "mixed-preview-pcm" && track !== "Mixed preview")
    .map(({ index }) => index);

  if (selectedAudioIndexes.length <= 1) {
    return {
      url: playbackPayloadUrl("/clip", { filePath }),
      mixed: false,
      message: "Playing original clip with range buffering."
    };
  }

  return {
    url: playbackPayloadUrl("/clip", { filePath }),
    mixed: true,
    message: "Streaming video with rolling mixed audio buffer.",
    audioChunkUrl: playbackPayloadUrl("/audio-chunk", { filePath, selectedAudioIndexes }),
    audioChunkSeconds: mixedAudioChunkSeconds
  };
}

function releasePlaybackCache(): boolean {
  return false;
}

async function clipThumbnailUrl(filePath: string): Promise<string> {
  if (!existsSync(filePath)) return "";
  const cacheKey = lowerPath(filePath);
  let signature = "";
  try {
    const fileStats = statSync(filePath);
    signature = `${fileStats.size}:${fileStats.mtimeMs}`;
  } catch {
    return "";
  }

  const cached = thumbnailDataUrlCache.get(cacheKey);
  if (cached?.signature === signature) {
    thumbnailDataUrlCache.delete(cacheKey);
    thumbnailDataUrlCache.set(cacheKey, cached);
    return cached.dataUrl;
  }
  thumbnailDataUrlCache.delete(cacheKey);

  const pending = pendingThumbnailTasks.get(cacheKey);
  if (pending) return pending;

  const task = thumbnailTaskLimiter.run(async () => {
    try {
      const image = await nativeImage.createThumbnailFromPath(filePath, {
        width: thumbnailWidth,
        height: thumbnailHeight
      });
      if (image.isEmpty()) return "";

      const jpeg = image.toJPEG(76);
      const dataUrl = `data:image/jpeg;base64,${jpeg.toString("base64")}`;
      thumbnailDataUrlCache.set(cacheKey, { signature, dataUrl });
      while (thumbnailDataUrlCache.size > thumbnailCacheLimit) {
        const oldestKey = thumbnailDataUrlCache.keys().next().value;
        if (oldestKey === undefined) break;
        thumbnailDataUrlCache.delete(oldestKey);
      }
      return dataUrl;
    } catch (err) {
      console.error(`Failed to generate thumbnail for ${filePath}:`, err);
      return "";
    }
  });
  pendingThumbnailTasks.set(cacheKey, task);
  return task.finally(() => pendingThumbnailTasks.delete(cacheKey));
}

function readSettings(): ClipSettings {
  const path = settingsPath();
  if (!existsSync(path)) {
    const saveFolder = join(app.getPath("videos"), "Clipture");
    return { ...defaultSettings, saveFolder };
  }
  return normalizeSettings({ ...defaultSettings, ...JSON.parse(readFileSync(path, "utf8")) });
}

function saveSettings(settings: ClipSettings): ClipSettings {
  const normalized = normalizeSettings(settings);
  mkdirSync(normalized.saveFolder, { recursive: true });
  writeFileSync(settingsPath(), JSON.stringify(normalized, null, 2));
  app.setLoginItemSettings({
    openAtLogin: normalized.startOnLogin,
    openAsHidden: true,
    args: app.isPackaged ? ["--hidden"] : [app.getAppPath(), "--hidden"],
  });
  void engine.configure(normalized).catch((error) => {
    console.error("Failed to configure engine:", error);
  });
  const hotkeyStatus = registerHotkey(normalized);
  if (hotkeyStatus) console.log(`[settings] ${hotkeyStatus}`);
  return normalized;
}

function normalizeAccelerator(hotkey: string): string {
  return hotkey
    .replace(/\bCtrl\b/gi, "Control")
    .replace(/\bCmdOrCtrl\b/gi, "CommandOrControl")
    .replace(/\s+/g, "");
}

function registerHotkey(settings: ClipSettings): string | null {
  const accelerator = normalizeAccelerator(settings.hotkey);
  if (currentHotkey === accelerator) return null;
  if (currentHotkey) globalShortcut.unregister(currentHotkey);
  currentHotkey = "";
  if (!accelerator) return "Hotkey is unassigned.";

  let registered = false;
  try {
    registered = globalShortcut.register(accelerator, () => {
      triggerSaveClipFromBackground();
    });
  } catch {
    registered = false;
  }

  if (registered) {
    currentHotkey = accelerator;
    return `Hotkey registered: ${settings.hotkey}`;
  }
  return `Hotkey failed to register: ${settings.hotkey}`;
}

const importedVideoExtensions = new Set([".mp4", ".m4v", ".mov", ".webm", ".mkv", ".avi"]);

function normalizedPathKey(filePath: string): string {
  return normalize(filePath).toLowerCase();
}

function importedClipId(filePath: string): string {
  return `imported:${createHash("sha1").update(normalizedPathKey(filePath)).digest("hex")}`;
}

function clipFolderName(filePath: string, settings = readSettings()): string {
  const parent = basename(dirname(filePath));
  const saveFolderName = basename(normalize(settings.saveFolder || ""));
  if (!parent || parent.toLowerCase() === saveFolderName.toLowerCase()) return "Clips";
  return parent;
}

function enrichSavedClip(clip: ClipRecord, settings = readSettings()): ClipRecord {
  return {
    ...clip,
    librarySource: "clip",
    folderName: clip.folderName || clipFolderName(clip.filePath, settings)
  };
}

async function scanImportedVideoFiles(root: string): Promise<string[]> {
  const files: string[] = [];
  const queue = [root];
  let scanned = 0;
  const maxEntries = 6000;

  while (queue.length > 0 && scanned < maxEntries) {
    const current = queue.shift()!;
    let entries;
    try {
      entries = await readdirAsync(current, { withFileTypes: true });
    } catch {
      continue;
    }

    for (const entry of entries) {
      if (++scanned > maxEntries) break;
      const fullPath = join(current, entry.name);
      if (entry.isDirectory()) {
        if (!entry.name.startsWith(".")) queue.push(fullPath);
      } else if (entry.isFile() && importedVideoExtensions.has(extname(entry.name).toLowerCase())) {
        files.push(fullPath);
      }
    }
  }

  return files;
}

type ImportedVideoIndexState = {
  rootsKey: string;
  clips: ClipRecord[];
  ready: boolean;
  generation: number;
  scan?: Promise<ClipRecord[]>;
};

let importedVideoIndexGeneration = 0;
let importedVideoIndex: ImportedVideoIndexState = { rootsKey: "", clips: [], ready: false, generation: importedVideoIndexGeneration };

function importedVideoRootsKey(settings: ClipSettings): string {
  return JSON.stringify((settings.importedVideoDirectories ?? []).map((root) => normalizedPathKey(root)));
}

function applyImportedVideoTitles(clips: ClipRecord[], settings: ClipSettings): ClipRecord[] {
  const titleOverrides = new Map(Object.entries(settings.importedVideoTitles ?? {}).map(([filePath, title]) => [normalizedPathKey(filePath), title]));
  return clips.map((clip) => ({
    ...clip,
    title: titleOverrides.get(normalizedPathKey(clip.filePath)) || parse(clip.filePath).name || "Imported video"
  }));
}

async function scanImportedVideoClips(settings: ClipSettings): Promise<ClipRecord[]> {
  const clips: ClipRecord[] = [];

  for (const root of settings.importedVideoDirectories ?? []) {
    const normalizedRoot = normalize(root);
    const folderName = basename(normalizedRoot) || "Imported videos";
    for (const filePath of await scanImportedVideoFiles(normalizedRoot)) {
      let stats;
      try {
        stats = await statAsync(filePath);
      } catch {
        continue;
      }
      const parsed = parse(filePath);
      clips.push({
        id: importedClipId(filePath),
        title: parsed.name || "Imported video",
        gameOrApp: folderName,
        librarySource: "imported",
        folderName,
        importedRoot: normalizedRoot,
        createdAt: new Date(stats.mtimeMs).toISOString(),
        durationSeconds: 0,
        filePath,
        resolution: "Imported",
        fps: 0,
        encoder: "Imported video",
        audioTracks: ["Imported audio"]
      });
    }
  }

  return clips.sort((a, b) => Number(new Date(b.createdAt)) - Number(new Date(a.createdAt)));
}

function invalidateImportedVideoIndex(): void {
  importedVideoIndexGeneration++;
  importedVideoIndex = { rootsKey: "", clips: [], ready: false, generation: importedVideoIndexGeneration };
}

function ensureImportedVideoIndex(settings: ClipSettings, delayMs = 2000): Promise<ClipRecord[]> {
  const rootsKey = importedVideoRootsKey(settings);
  if (importedVideoIndex.rootsKey !== rootsKey) {
    importedVideoIndexGeneration++;
    importedVideoIndex = { rootsKey, clips: [], ready: false, generation: importedVideoIndexGeneration };
  }
  if (importedVideoIndex.ready) return Promise.resolve(importedVideoIndex.clips);
  if (importedVideoIndex.scan) return importedVideoIndex.scan;

  const settingsSnapshot: ClipSettings = {
    ...settings,
    importedVideoDirectories: [...(settings.importedVideoDirectories ?? [])],
    importedVideoTitles: { ...(settings.importedVideoTitles ?? {}) },
    audioSources: [...settings.audioSources]
  };
  const generation = importedVideoIndex.generation;
  const scan = new Promise<void>((resolve) => setTimeout(resolve, Math.max(0, delayMs)))
    .then(() => scanImportedVideoClips(settingsSnapshot))
    .then((clips) => {
      if (importedVideoIndex.rootsKey !== rootsKey || importedVideoIndex.generation !== generation) return clips;
      importedVideoIndex = { rootsKey, clips, ready: true, generation };
      mainWindow?.webContents.send("library:changed");
      return clips;
    })
    .catch((error) => {
      console.error("Failed to index imported video folders:", error);
      if (importedVideoIndex.rootsKey === rootsKey && importedVideoIndex.generation === generation) {
        importedVideoIndex = { rootsKey, clips: [], ready: true, generation };
      }
      return [];
    });
  importedVideoIndex.scan = scan;
  return scan;
}

async function getImportedVideoClips(settings = readSettings()): Promise<ClipRecord[]> {
  const clips = await ensureImportedVideoIndex(settings, 0);
  return applyImportedVideoTitles(clips, settings);
}

function readClips(): ClipRecord[] {
  const path = clipsPath();
  if (!existsSync(path)) return [];
  const settings = readSettings();
  const clips = JSON.parse(readFileSync(path, "utf8")) as ClipRecord[];
  const existingClips = clips.filter((clip) => existsSync(clip.filePath));
  if (existingClips.length !== clips.length) {
    writeClips(existingClips);
    mainWindow?.webContents.send("library:changed");
  }
  return existingClips.map((clip) => enrichSavedClip(clip, settings));
}

function listLibraryClips(): ClipRecord[] {
  const settings = readSettings();
  const rootsKey = importedVideoRootsKey(settings);
  const importedClips = importedVideoIndex.rootsKey === rootsKey && importedVideoIndex.ready
    ? applyImportedVideoTitles(importedVideoIndex.clips, settings)
    : [];
  void ensureImportedVideoIndex(settings).catch(() => {});
  return [
    ...readClips(),
    ...importedClips
  ];
}

function writeClips(clips: ClipRecord[]): void {
  writeFileSync(clipsPath(), JSON.stringify(clips.filter((clip) => clip.librarySource !== "imported"), null, 2));
}

async function deleteClips(ids: string[]): Promise<boolean> {
  const idSet = new Set(ids);
  if (idSet.size === 0) return false;
  const importedIds = [...idSet].filter((id) => id.startsWith("imported:"));
  let removedImported = false;
  if (importedIds.length > 0) {
    const settings = readSettings();
    const importedClips = await getImportedVideoClips(settings);
    const titleOverrides = { ...(settings.importedVideoTitles ?? {}) };
    for (const clip of importedClips) {
      if (!idSet.has(clip.id)) continue;
      const normalizedFilePath = normalize(clip.filePath);
      try {
        if (existsSync(clip.filePath)) unlinkSync(clip.filePath);
        delete titleOverrides[normalizedFilePath];
        removedImported = true;
      } catch (error) {
        console.error("Failed to delete imported video file:", error);
      }
    }
    if (removedImported) {
      saveSettings({ ...settings, importedVideoTitles: titleOverrides });
      invalidateImportedVideoIndex();
      void ensureImportedVideoIndex(readSettings(), 250).catch(() => {});
    }
  }

  const clips = readClips();
  const keptClips: ClipRecord[] = [];
  let deleted = false;

  for (const clip of clips) {
    if (!idSet.has(clip.id)) {
      keptClips.push(clip);
      continue;
    }

    deleted = true;
    const files = [clip.filePath, ...(clip.segmentFiles ?? [])];
    for (const file of files) {
      try {
        if (existsSync(file)) unlinkSync(file);
      } catch (error) {
        console.error("Failed to delete clip file:", error);
      }
    }
  }

  if (deleted) writeClips(keptClips);
  if (deleted || removedImported) mainWindow?.webContents.send("library:changed");
  return deleted || removedImported;
}

async function importVideoFolders(): Promise<boolean> {
  const settings = readSettings();
  const options: OpenDialogOptions = {
    title: "Import video folders",
    defaultPath: settings.saveFolder || app.getPath("videos"),
    properties: ["openDirectory", "multiSelections"]
  };
  const result = mainWindow
    ? await dialog.showOpenDialog(mainWindow, options)
    : await dialog.showOpenDialog(options);
  if (result.canceled || result.filePaths.length === 0) return false;

  const directories = Array.from(new Set([
    ...(settings.importedVideoDirectories ?? []),
    ...result.filePaths
  ].map((value) => normalize(value))));
  saveSettings({ ...settings, importedVideoDirectories: directories });
  invalidateImportedVideoIndex();
  void ensureImportedVideoIndex(readSettings(), 250).catch(() => {});
  mainWindow?.webContents.send("library:changed");
  return true;
}

function triggerSaveClipFromBackground(): void {
  void saveClipAndRecord(readSettings().clipLengthSeconds).then((result) => {
    if (!result.ok) console.warn(`[clip] ${result.message}`);
  }).catch((error) => {
    console.error("Failed to save clip:", error);
  });
}

async function saveClipAndRecord(durationSeconds = readSettings().clipLengthSeconds): Promise<SaveClipResult> {
  const saveId = `save-${Date.now().toString(36)}`;
  if (saveClipInProgress) {
    logSaveTimingLine(`[save-timing] id=${saveId} stage=rejected reason="in_progress"`);
    return { ok: false, message: "A clip is already being saved. Wait for it to finish before saving another one." };
  }

  saveClipInProgress = true;
  const totalStartedAt = saveTimingNowMs();
  let finalStatus = "unknown";
  try {
    const settings = readSettings();
    logSaveTiming(saveId, "start", totalStartedAt, {
      durationSeconds,
      resolutionPreset: settings.resolutionPreset,
      fps: settings.fps,
      audioSources: settings.audioSources.filter((source) => source.enabled).length
    });

    if (settings.clipSound && settings.clipSound !== "none") {
      mainWindow?.webContents.send("play-sound", settings.clipSound);
    }

    if (settings.showNotification && notificationWindow) {
      showNotificationWindow(settings.notificationPosition || "top-right");
      notificationWindow.webContents.send("show-notification", "", settings.notificationPosition || "top-right", "Saving clip...");
    }

    const engineStartedAt = saveTimingNowMs();
    const result = await engine.saveClip(durationSeconds);
    logSaveTiming(saveId, "engine.saveClip", engineStartedAt, { ok: result.ok });
    if (result.ok && result.clip) {
      const presetResolution = recordingOutputResolution(settings);
      const actualResolution = parseResolutionLabel(result.clip.resolution);
      const targetResolution = presetResolution.width > 0 && presetResolution.height > 0
        ? presetResolution
        : actualResolution.width > 0 && actualResolution.height > 0
          ? actualResolution
          : parseResolutionLabel(result.clip.recommendedResolution);
      const targetResolutionLabel = targetResolution.width > 0 && targetResolution.height > 0
        ? `${targetResolution.width}x${targetResolution.height}`
        : "";
      const reencodeBitrateMbps = settings.autoBitrate
        ? autoBitrateForResolution(targetResolution.width > 0 && targetResolution.height > 0 ? targetResolution : parseResolutionLabel(result.clip.resolution), settings.fps, settings.maxAutoBitrateMbps)
        : settings.bitrateMbps;

      if (result.clip.segmentFiles && result.clip.segmentFiles.length > 1) {
        const stitchStartedAt = saveTimingNowMs();
        const stitchResult = await stitchSegmentedClip(result.clip, targetResolution, reencodeBitrateMbps, saveId);
        logSaveTiming(saveId, "stitch.call", stitchStartedAt, { ok: stitchResult.ok });
        result.message = `${result.message} ${stitchResult.message}`;
        if (!stitchResult.ok) {
          result.ok = false;
          finalStatus = "stitch_failed";
          return result;
        }
      }

      const processStartedAt = saveTimingNowMs();
      const processResult = await processClipFile(result.clip.filePath, targetResolution, actualResolution, reencodeBitrateMbps, result.clip.audioTracks, saveId);
      logSaveTiming(saveId, "postprocess.call", processStartedAt, { ok: processResult.ok });

      if (processResult.ok) {
        if (targetResolutionLabel) {
          result.clip.resolution = targetResolutionLabel;
        }
        if (processResult.tracksUpdated) {
          result.clip.audioTracks = processResult.newTracks;
        }
        result.message = `${result.message} ${processResult.message}`;
      } else {
        result.message = `${result.message} Processing failed: ${processResult.message}`;
      }

      const finalizeStartedAt = saveTimingNowMs();
      let gameFolder = result.clip.gameOrApp || "Other";
      const lowerGame = gameFolder.toLowerCase();

      if (lowerGame.includes("explorer") || lowerGame.includes("desktop")) {
        gameFolder = "Explorer";
      } else if (!result.clip.isGame) {
        gameFolder = "Apps";
      } else {
        gameFolder = gameFolder.replace(/[\\/:*?"<>|]/g, "_").trim() || "Other";
      }

      const currentDir = dirname(result.clip.filePath);
      const newDir = join(currentDir, gameFolder);
      if (!existsSync(newDir)) {
        try { mkdirSync(newDir, { recursive: true }); } catch (e) { /* ignore */ }
      }

      const newFilePath = join(newDir, basename(result.clip.filePath));
      const shouldMoveClip = newFilePath !== result.clip.filePath && existsSync(result.clip.filePath);
      let movedClip = false;
      if (shouldMoveClip) {
        try {
          renameSync(result.clip.filePath, newFilePath);
          result.clip.filePath = newFilePath;
          movedClip = true;
        } catch (e) {
          console.error("Failed to move clip to game subfolder:", e);
        }
      }

      const clips = readClips();
      clips.unshift(result.clip);
      writeClips(clips);
      mainWindow?.webContents.send("library:changed");
      logSaveTiming(saveId, "finalize", finalizeStartedAt, {
        moved: movedClip,
        filePath: result.clip.filePath
      });

      if (settings.showNotification && notificationWindow) {
        showNotificationWindow(settings.notificationPosition || "top-right");
        notificationWindow.webContents.send("show-notification", "", settings.notificationPosition || "top-right", "Clip saved!");
      }
    } else if (settings.showNotification && notificationWindow) {
      showNotificationWindow(settings.notificationPosition || "top-right");
      notificationWindow.webContents.send("show-notification", "", settings.notificationPosition || "top-right", "Clip failed");
    }
    finalStatus = result.ok ? "ok" : "failed";
    return result;
  } catch (error) {
    console.error("Failed to save clip:", error);
    finalStatus = "exception";
    const settings = readSettings();
    if (settings.showNotification && notificationWindow) {
      showNotificationWindow(settings.notificationPosition || "top-right");
      notificationWindow.webContents.send("show-notification", "", settings.notificationPosition || "top-right", "Clip failed");
    }
    const message = error instanceof Error ? error.message : "Could not save clip.";
    return {
      ok: false,
      message: message.includes("Engine request timed out: saveClip")
        ? "Clip save is taking longer than expected. Wait a moment and try again; the engine may still be finishing the previous save."
        : message
    };
  } finally {
    logSaveTiming(saveId, "total", totalStartedAt, { status: finalStatus });
    saveClipInProgress = false;
  }
}

function resolveEnginePath(): string | undefined {
  const candidates = [
    join(process.cwd(), "build", "engine", "Release", "clipture_engine.exe"),
    join(process.cwd(), "build", "engine", "Debug", "clipture_engine.exe"),
    join(process.resourcesPath ?? "", "engine", "clipture_engine.exe")
  ];
  return candidates.find((candidate) => existsSync(candidate));
}

function resolveAssetPath(fileName: string): string | undefined {
  const candidates = [
    join(process.resourcesPath ?? "", "assets", fileName),
    join(process.cwd(), "assets", fileName),
    join(__dirname, "../../assets", fileName)
  ];
  return candidates.find((candidate) => existsSync(candidate));
}

async function createWindow(): Promise<void> {
  mainWindow = new BrowserWindow({
    width: 1240,
    height: 780,
    minWidth: 980,
    minHeight: 640,
    show: false,
    title: "Clipture",
    icon: resolveAssetPath("icon.ico"),
    backgroundColor: "#101114",
    titleBarStyle: "hidden",
    titleBarOverlay: {
      color: "#101114",
      symbolColor: "#a5adba",
      height: 32
    },
    webPreferences: {
      preload: join(__dirname, "../preload/preload.js"),
      contextIsolation: true,
      nodeIntegration: false
    }
  });

  mainWindow.setMenu(null);

  mainWindow.once("ready-to-show", () => {
    if (!startHidden) mainWindow?.show();
  });
  mainWindow.on("close", (event) => {
    if (isQuitting) return;
    event.preventDefault();
    mainWindow?.hide();
  });

  const devUrl = process.env.VITE_DEV_SERVER_URL;
  if (devUrl) await mainWindow.loadURL(devUrl);
  else await mainWindow.loadFile(join(__dirname, "../renderer/index.html"));
}

async function createNotificationWindow(): Promise<void> {
  notificationWindow = new BrowserWindow({
    width: 350,
    height: 100,
    transparent: true,
    backgroundColor: '#00000000',
    frame: false,
    alwaysOnTop: true,
    skipTaskbar: true,
    focusable: false,
    hasShadow: false,
    show: false,
    resizable: false,
    webPreferences: {
      preload: join(__dirname, "../preload/preload.js"),
      contextIsolation: true,
      nodeIntegration: false
    }
  });

  notificationWindow.setIgnoreMouseEvents(true, { forward: true });
  notificationWindow.setMenu(null);

  ipcMain.on("hide-notification", () => {
    notificationWindow?.hide();
  });

  const devUrl = process.env.VITE_DEV_SERVER_URL;
  if (devUrl) await notificationWindow.loadURL(devUrl + "#notification");
  else {
    const fileUrl = pathToFileURL(join(__dirname, "../renderer/index.html"));
    fileUrl.hash = "notification";
    await notificationWindow.loadURL(fileUrl.href);
  }
}

function showNotificationWindow(position: string): void {
  if (!notificationWindow) return;
  const workArea = screen.getPrimaryDisplay().workArea;
  const winWidth = 350;
  const winHeight = 100;
  const xPadding = 0; // 0px to touch the edge of the screen
  const yPadding = 30; // 30px from top/bottom

  let x = workArea.x;
  let y = workArea.y;

  if (position.includes('right')) x = workArea.x + workArea.width - winWidth - xPadding;
  else if (position.includes('left')) x = workArea.x + xPadding;
  else if (position.includes('center')) x = workArea.x + (workArea.width / 2) - (winWidth / 2);

  if (position.includes('top')) {
    y = workArea.y + yPadding;
  }
  else if (position.includes('bottom')) {
    y = workArea.y + workArea.height - winHeight - yPadding;
  }

  notificationWindow.setBounds({ x: Math.round(x), y: Math.round(y), width: winWidth, height: winHeight });
  notificationWindow.showInactive();
  notificationWindow.setIgnoreMouseEvents(true, { forward: true });
}

function createTray(): void {
  const icoPath = resolveAssetPath("icon.ico");
  const svgPath = resolveAssetPath("svgviewer-output.svg");
  const iconSource = icoPath || svgPath;
  const loadedIcon = iconSource ? nativeImage.createFromPath(iconSource) : nativeImage.createEmpty();
  const icon = loadedIcon.isEmpty() ? loadedIcon : loadedIcon.resize({ width: 16, height: 16 });
  icon.setTemplateImage(false);
  tray = new Tray(icon);
  tray.setToolTip("Clipture");
  tray.setContextMenu(
    Menu.buildFromTemplate([
      { label: "Open Clipture", click: () => mainWindow?.show() },
      { label: "Save Clip", click: () => triggerSaveClipFromBackground() },
      { label: "Open Clips Folder", click: () => shell.openPath(readSettings().saveFolder) },
      { type: "separator" },
      { label: "Exit", click: () => app.quit() }
    ])
  );
}

const gotSingleInstanceLock = app.requestSingleInstanceLock();
if (!gotSingleInstanceLock) {
  app.quit();
}

app.on("second-instance", (_event, commandLine) => {
  if (commandLine.includes("--hidden") || commandLine.includes("--background")) return;
  if (!mainWindow) return;
  if (mainWindow.isMinimized()) mainWindow.restore();
  mainWindow.show();
  mainWindow.focus();
});

app.whenReady().then(async () => {
  const startupSettings = readSettings();
  cleanupAbandonedPreviewCache();
  setupPreviewServer();
  ensureBundledClipSounds();
  createTray();
  await createWindow();
  await createNotificationWindow();
  registerHotkey(startupSettings);
  setTimeout(() => {
    void engine.configure(startupSettings).catch((error) => {
      console.error("Failed to configure engine at startup:", error);
    });
  }, 250);
  setTimeout(() => checkForAppUpdates(), 4000);
});

app.on("window-all-closed", () => {});

app.on("before-quit", () => {
  isQuitting = true;
  if (updateCheckTimer) clearInterval(updateCheckTimer);
  globalShortcut.unregisterAll();
  engine.stop();
});

ipcMain.handle("engine:getDiagnostics", () => engine.diagnostics());
ipcMain.handle("engine:saveClip", async (_event, durationSeconds: number) => {
  return await saveClipAndRecord(durationSeconds);
});
ipcMain.handle("settings:get", () => readSettings());
ipcMain.handle("settings:save", (_event, settings: ClipSettings) => saveSettings(settings));
ipcMain.handle("library:list", () => listLibraryClips());
ipcMain.handle("library:delete", (_event, ids: string[]) => deleteClips(Array.isArray(ids) ? ids : []));
ipcMain.handle("library:importVideoFolders", () => importVideoFolders());
ipcMain.handle("library:rename", async (_event, id: string, newTitle: string) => {
  const trimmedTitle = typeof newTitle === "string" ? newTitle.trim() : "";
  if (!trimmedTitle) return false;
  if (id.startsWith("imported:")) {
    const settings = readSettings();
    const importedClip = (await getImportedVideoClips(settings)).find((clip) => clip.id === id);
    if (!importedClip || !existsSync(importedClip.filePath)) return false;
    const safeTitle = trimmedTitle.replace(/[\\/:*?"<>|]/g, "_").trim() || "video";
    const dir = dirname(importedClip.filePath);
    const ext = extname(importedClip.filePath) || ".mp4";
    let newFilePath = join(dir, `${safeTitle}${ext}`);
    let counter = 1;
    while (existsSync(newFilePath) && normalizedPathKey(newFilePath) !== normalizedPathKey(importedClip.filePath)) {
      newFilePath = join(dir, `${safeTitle}_${counter}${ext}`);
      counter++;
    }

    try {
      if (normalizedPathKey(newFilePath) !== normalizedPathKey(importedClip.filePath)) {
        renameSync(importedClip.filePath, newFilePath);
      }
    } catch (error) {
      console.error("Failed to rename imported video file:", error);
      return false;
    }

    const titleOverrides = { ...(settings.importedVideoTitles ?? {}) };
    delete titleOverrides[normalize(importedClip.filePath)];
    titleOverrides[normalize(newFilePath)] = trimmedTitle;
    saveSettings({
      ...settings,
      importedVideoTitles: titleOverrides
    });
    invalidateImportedVideoIndex();
    void ensureImportedVideoIndex(readSettings(), 250).catch(() => {});
    mainWindow?.webContents.send("library:changed");
    return true;
  }

  const clips = readClips();
  const clip = clips.find((c) => c.id === id);
  if (clip) {
    clip.title = trimmedTitle;
    
    // Rename file on disk
    if (existsSync(clip.filePath)) {
      const safeTitle = trimmedTitle.replace(/[\\/:*?"<>|]/g, "_").trim() || "clip";
      const dir = dirname(clip.filePath);
      const ext = clip.filePath.substring(clip.filePath.lastIndexOf("."));
      let newFilePath = join(dir, `${safeTitle}${ext}`);
      let counter = 1;
      
      while (existsSync(newFilePath) && newFilePath !== clip.filePath) {
        newFilePath = join(dir, `${safeTitle}_${counter}${ext}`);
        counter++;
      }
      
      if (newFilePath !== clip.filePath) {
        try {
          renameSync(clip.filePath, newFilePath);
          clip.filePath = newFilePath;
        } catch (e) {
          console.error("Failed to rename clip file:", e);
        }
      }
    }
    
    writeClips(clips);
    mainWindow?.webContents.send("library:changed");
    return true;
  }
  return false;
});
ipcMain.handle("library:clipUrl", (_event, filePath: string) => (existsSync(filePath) ? pathToFileURL(filePath).toString() : ""));
ipcMain.handle("library:clipIconUrl", (_event, clip: ClipRecord, preferredLabels?: string[]) => clipIconUrl(clip, preferredLabels ?? []));
ipcMain.handle("library:clipThumbnailUrl", (_event, filePath: string) => clipThumbnailUrl(filePath));
ipcMain.handle("library:clipPlaybackUrl", (_event, filePath: string, audioTracks: string[]) => clipPlaybackUrl(filePath, audioTracks));
ipcMain.handle("library:releasePlaybackCache", () => releasePlaybackCache());
ipcMain.handle("updates:getState", () => updateState);
ipcMain.handle("updates:check", () => performUpdateCheck());
ipcMain.handle("updates:download", () => {
  setUpdateState({ ...updateState, status: "downloading", message: "Starting download..." });
  return autoUpdater.downloadUpdate();
});
ipcMain.handle("updates:install", () => installDownloadedUpdate());
ipcMain.handle("processes:list", () => listActiveProcesses());
ipcMain.handle("processes:iconUrl", (_event, processName: string, executablePath?: string) => processIconUrl(processName, executablePath));
ipcMain.handle("audio:listInputDevices", () => engine.listAudioInputDevices());
ipcMain.handle("displays:list", () => engine.listDisplayDevices());
ipcMain.handle("sounds:list", () => listClipSounds());
ipcMain.handle("sounds:import", async () => {
  const options: OpenDialogOptions = {
    title: "Import clip sound",
    properties: ["openFile"],
    filters: [{ name: "Audio", extensions: ["mp3", "wav", "ogg"] }]
  };
  const result = mainWindow
    ? await dialog.showOpenDialog(mainWindow, options)
    : await dialog.showOpenDialog(options);
  if (result.canceled || result.filePaths.length === 0) return undefined;
  const sourcePath = result.filePaths[0];
  const destinationPath = uniqueSoundPath(sourcePath);
  copyFileSync(sourcePath, destinationPath);
  const fileName = basename(destinationPath);
  return {
    id: `custom:${fileName}`,
    label: labelFromSoundFileName(fileName),
    url: pathToFileURL(destinationPath).toString(),
    builtIn: false
  } satisfies ClipSoundOption;
});
ipcMain.handle("sounds:reveal", async () => {
  await shell.openPath(soundsFolderPath());
});
ipcMain.handle("library:reveal", async (_event, filePath: string) => {
  shell.showItemInFolder(filePath);
});
ipcMain.handle("dialog:selectFolder", async (_event, currentPath: string) => {
  if (!mainWindow) return currentPath;
  const result = await dialog.showOpenDialog(mainWindow, {
    title: "Select Save Folder",
    defaultPath: currentPath || app.getPath("videos"),
    properties: ["openDirectory"]
  });
  if (result.canceled || result.filePaths.length === 0) {
    return currentPath;
  }
  return result.filePaths[0];
});
