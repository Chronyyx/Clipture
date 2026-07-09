import { app, BrowserWindow, globalShortcut, ipcMain, shell, Tray, Menu, nativeImage, dialog, screen } from "electron";
import type { OpenDialogOptions } from "electron";
import electronUpdater from "electron-updater";
import { spawn, ChildProcessWithoutNullStreams } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, renameSync, statSync, unlinkSync, writeFileSync } from "node:fs";
import { basename, dirname, extname, join, parse } from "node:path";
import { pathToFileURL } from "node:url";
import { createHash } from "node:crypto";
import type { ActiveProcess, AudioInputDevice, ClipRecord, ClipSettings, DisplayDevice, EngineDiagnostics, SaveClipResult, ClipSoundOption } from "../shared/types";

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
  private pending = new Map<number, { resolve: (value: unknown) => void; reject: (error: Error) => void }>();
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
    this.child.stderr.on("data", (chunk: Buffer) => console.error(`[engine] ${chunk.toString()}`));
    this.child.on("exit", () => {
      this.child = undefined;
      this.lastDiagnostics = { ...this.lastDiagnostics, activeEncoder: "Unavailable", encoderMode: "Unavailable", engineRunning: false, degraded: true, status: "Native engine exited." };
      for (const pending of this.pending.values()) pending.reject(new Error("Native engine exited."));
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
    return this.request<SaveClipResult>("saveClip", { durationSeconds, saveFolder }, 60000);
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
      appAudioProcesses: Array.from(new Set([
        ...settings.audioSources
          .filter((source) => source.kind === "app" && source.enabled && source.processName?.trim())
          .map((source) => source.processName!.trim()),
        ...settings.audioSources
          .filter((source) => source.id === "system" && source.enabled && !(source.captureAllSystem ?? true))
          .flatMap((source) => source.processNames || [])
          .map((name) => name.trim())
      ])).join("|"),
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
    });
    this.lastDiagnostics = diagnostics;
    return diagnostics;
  }

  private request<T>(type: string, payload: Record<string, unknown>, timeoutMs: number = 5000): Promise<T> {
    if (!this.child) return Promise.reject(new Error("Native engine is not running."));
    const id = this.nextId++;
    const message = JSON.stringify({ id, type, ...payload });
    this.child.stdin.write(`${message}\n`);
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve: resolve as (value: unknown) => void, reject });
      setTimeout(() => {
        if (!this.pending.has(id)) return;
        this.pending.delete(id);
        reject(new Error(`Engine request timed out: ${type}`));
      }, timeoutMs);
    });
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

app.setName("Clipture");
if (process.platform === "win32") {
  app.setAppUserModelId("app.clipture.desktop");
}

function checkForAppUpdates(): void {
  if (!app.isPackaged) return;
  const { autoUpdater } = electronUpdater;
  autoUpdater.on("error", (error) => {
    console.error("[updates]", error);
  });
  autoUpdater.on("update-downloaded", async (updateInfo) => {
    const result = await dialog.showMessageBox({
      type: "info",
      buttons: ["Restart now", "Later"],
      defaultId: 0,
      cancelId: 1,
      title: "Update ready",
      message: `Clipture ${updateInfo.version} is ready to install.`,
      detail: "Restart Clipture to finish updating."
    });
    if (result.response === 0) {
      isQuitting = true;
      autoUpdater.quitAndInstall();
    }
  });
  void autoUpdater.checkForUpdatesAndNotify().catch((error) => {
    console.error("[updates]", error);
  });
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

function previewPath(filePath: string, audioTracks: string[]): string {
  const previewDir = appDataPath("previews");
  mkdirSync(previewDir, { recursive: true });
  const stats = statSync(filePath);
  const hash = createHash("sha1")
    .update(filePath)
    .update(String(stats.mtimeMs))
    .update(String(stats.size))
    .update(audioTracks.join("|"))
    .digest("hex");
  return join(previewDir, `${hash}.mp4`);
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

function listActiveProcesses(): Promise<ActiveProcess[]> {
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
  audioTracks: string[]
): Promise<{ ok: boolean; message: string; tracksUpdated: boolean; newTracks: string[] }> {
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

  if (!existsSync(filePath)) return { ok: true, message: "Clip file was not found.", tracksUpdated: false, newTracks: audioTracks };

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

  const ffmpeg = await runFfmpeg(args);
  if (!ffmpeg.ok || !existsSync(outputPath)) return { ok: ffmpeg.ok, message: ffmpeg.message, tracksUpdated: false, newTracks: audioTracks };

  try {
    unlinkSync(filePath);
    renameSync(outputPath, filePath);
  } catch (error) {
    return { ok: false, message: error instanceof Error ? error.message : "Could not replace clip with processed output.", tracksUpdated: false, newTracks: audioTracks };
  }

  return { ok: true, message: needsScale && needsMix ? "Scaled and mixed audio." : needsScale ? "Scaled video." : "Mixed system audio.", tracksUpdated: needsMix, newTracks };
}

async function stitchSegmentedClip(
  clip: ClipRecord,
  target: { width: number; height: number },
  reencodeBitrateMbps: number
): Promise<{ ok: boolean; message: string }> {
  const segmentFiles = clip.segmentFiles?.filter((file) => existsSync(file)) ?? [];
  if (segmentFiles.length <= 1) return { ok: true, message: "No segment stitching needed." };

  const targetWidth = target.width > 0 ? target.width : parseResolutionLabel(clip.recommendedResolution).width;
  const targetHeight = target.height > 0 ? target.height : parseResolutionLabel(clip.recommendedResolution).height;
  if (targetWidth <= 0 || targetHeight <= 0) {
    return { ok: false, message: "Segment stitching needs a target resolution." };
  }

  const segmentResolutions = clip.segmentResolutions?.map(parseResolutionLabel) ?? [];
  const canUseGpuScale =
    segmentResolutions.length === segmentFiles.length &&
    segmentResolutions.every((resolution) =>
      resolution.width > 0 &&
      resolution.height > 0 &&
      resolution.width * targetHeight === resolution.height * targetWidth
    );
  const filterSupport = canUseGpuScale ? await queryFfmpegFilterSupport() : { scaleCuda: false, scaleNpp: false };

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
    ffmpeg = await runFfmpeg(buildArgs(mode));
    if (ffmpeg.ok && existsSync(clip.filePath)) break;
    try {
      if (existsSync(clip.filePath)) unlinkSync(clip.filePath);
    } catch {
      // Retry cleanup is best effort.
    }
  }
  if (!ffmpeg.ok || !existsSync(clip.filePath)) return { ok: ffmpeg.ok, message: ffmpeg.message };

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
  return { ok: true, message: usedMode === "cpu" ? "Stitched segmented video." : `Stitched segmented video with GPU ${usedMode} scaling.` };
}

async function clipPlaybackUrl(filePath: string, audioTracks: string[]): Promise<{ url: string; mixed: boolean; message: string }> {
  if (!existsSync(filePath)) return { url: "", mixed: false, message: "Clip file was not found." };

  const selectedAudioIndexes = audioTracks
    .map((track, index) => ({ track, index }))
    .filter(({ track }) => track !== "mixed-preview-pcm" && track !== "Mixed preview")
    .map(({ index }) => index);

  if (selectedAudioIndexes.length <= 1) {
    return { url: pathToFileURL(filePath).toString(), mixed: false, message: "Playing original clip audio track." };
  }

  const outputPath = previewPath(filePath, audioTracks);
  if (existsSync(outputPath)) {
    return { url: pathToFileURL(outputPath).toString(), mixed: true, message: "Playing cached mixed preview." };
  }

  const filterInputs = selectedAudioIndexes.map((index) => `[0:a:${index}]`).join("");
  const filter = `${filterInputs}amix=inputs=${selectedAudioIndexes.length}:duration=longest:dropout_transition=0[aout]`;
  const ffmpeg = await runFfmpeg([
    "-y",
    "-hide_banner",
    "-loglevel",
    "error",
    "-i",
    filePath,
    "-filter_complex",
    filter,
    "-map",
    "0:v:0",
    "-map",
    "[aout]",
    "-c:v",
    "copy",
    "-c:a",
    "aac",
    "-b:a",
    "192k",
    "-movflags",
    "+faststart",
    outputPath
  ]);

  if (!ffmpeg.ok || !existsSync(outputPath)) {
    if (existsSync(outputPath)) {
      try {
        unlinkSync(outputPath);
      } catch {
        // Best effort cleanup; playback can still fall back to the original clip.
      }
    }
    return { url: pathToFileURL(filePath).toString(), mixed: false, message: ffmpeg.message };
  }

  return { url: pathToFileURL(outputPath).toString(), mixed: true, message: ffmpeg.message };
}

async function clipThumbnailUrl(filePath: string): Promise<string> {
  if (!existsSync(filePath)) return "";

  const outputPath = mediaCachePath(filePath, "thumbnails", "jpg");
  if (existsSync(outputPath)) return pathToFileURL(outputPath).toString();

  const ffmpeg = await runFfmpeg([
    "-y",
    "-hide_banner",
    "-loglevel",
    "error",
    "-ss",
    "00:00:01",
    "-i",
    filePath,
    "-frames:v",
    "1",
    "-vf",
    "scale=640:-2",
    "-q:v",
    "3",
    outputPath
  ]);

  if (!ffmpeg.ok || !existsSync(outputPath)) {
    if (existsSync(outputPath)) {
      try {
        unlinkSync(outputPath);
      } catch {
        // Best effort cleanup; the renderer can show its thumbnail fallback.
      }
    }
    return "";
  }

  return pathToFileURL(outputPath).toString();
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
  app.setLoginItemSettings({ openAtLogin: normalized.startOnLogin, openAsHidden: true, args: ["--hidden"] });
  void engine.configure(normalized);
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
    registered = globalShortcut.register(accelerator, async () => {
      await saveClipAndRecord(readSettings().clipLengthSeconds);
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

function readClips(): ClipRecord[] {
  const path = clipsPath();
  if (!existsSync(path)) return [];
  const clips = JSON.parse(readFileSync(path, "utf8")) as ClipRecord[];
  const existingClips = clips.filter((clip) => existsSync(clip.filePath));
  if (existingClips.length !== clips.length) {
    writeClips(existingClips);
    mainWindow?.webContents.send("library:changed");
  }
  return existingClips;
}

function writeClips(clips: ClipRecord[]): void {
  writeFileSync(clipsPath(), JSON.stringify(clips, null, 2));
}

async function saveClipAndRecord(durationSeconds = readSettings().clipLengthSeconds): Promise<SaveClipResult> {
  const settings = readSettings();
  if (settings.clipSound && settings.clipSound !== 'none') {
    mainWindow?.webContents.send("play-sound", settings.clipSound);
  }
  
  if (settings.showNotification && notificationWindow) {
    showNotificationWindow(settings.notificationPosition || 'top-right');
    notificationWindow.webContents.send("show-notification", "", settings.notificationPosition || 'top-right');
  }

  const result = await engine.saveClip(durationSeconds);
  if (result.ok && result.clip) {
    const presetResolution = recordingOutputResolution(settings);
    const targetResolution = presetResolution.width > 0 && presetResolution.height > 0
      ? presetResolution
      : parseResolutionLabel(result.clip.recommendedResolution);
    const targetResolutionLabel = targetResolution.width > 0 && targetResolution.height > 0
      ? `${targetResolution.width}x${targetResolution.height}`
      : "";
    const reencodeBitrateMbps = settings.autoBitrate
      ? autoBitrateForResolution(targetResolution.width > 0 && targetResolution.height > 0 ? targetResolution : parseResolutionLabel(result.clip.resolution), settings.fps, settings.maxAutoBitrateMbps)
      : settings.bitrateMbps;

    if (result.clip.segmentFiles && result.clip.segmentFiles.length > 1) {
      const stitchResult = await stitchSegmentedClip(result.clip, targetResolution, reencodeBitrateMbps);
      result.message = `${result.message} ${stitchResult.message}`;
      if (!stitchResult.ok) {
        result.ok = false;
        return result;
      }
    }

    const actualResolution = parseResolutionLabel(result.clip.resolution);
    const processResult = await processClipFile(result.clip.filePath, targetResolution, actualResolution, reencodeBitrateMbps, result.clip.audioTracks);
    
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
    if (newFilePath !== result.clip.filePath && existsSync(result.clip.filePath)) {
      try {
        renameSync(result.clip.filePath, newFilePath);
        result.clip.filePath = newFilePath;
      } catch (e) {
        console.error("Failed to move clip to game subfolder:", e);
      }
    }
    
    const clips = readClips();
    clips.unshift(result.clip);
    writeClips(clips);
    mainWindow?.webContents.send("library:changed");
  }
  return result;
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
  const svgPath = resolveAssetPath("svgviewer-output.svg");
  const icoPath = resolveAssetPath("icon.ico");
  const iconSource = svgPath || icoPath;
  const loadedIcon = iconSource ? nativeImage.createFromPath(iconSource) : nativeImage.createEmpty();
  const icon = loadedIcon.isEmpty() ? loadedIcon : loadedIcon.resize({ width: 16, height: 16 });
  icon.setTemplateImage(false);
  tray = new Tray(icon);
  tray.setToolTip("Clipture");
  tray.setContextMenu(
    Menu.buildFromTemplate([
      { label: "Open Clipture", click: () => mainWindow?.show() },
      { label: "Save Clip", click: async () => saveClipAndRecord(readSettings().clipLengthSeconds) },
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
  ensureBundledClipSounds();
  saveSettings(readSettings());
  engine.start();
  createTray();
  await createWindow();
  await createNotificationWindow();
  registerHotkey(readSettings());
  checkForAppUpdates();
});

app.on("window-all-closed", () => {});

app.on("before-quit", () => {
  isQuitting = true;
  globalShortcut.unregisterAll();
  engine.stop();
});

ipcMain.handle("engine:getDiagnostics", () => engine.diagnostics());
ipcMain.handle("engine:saveClip", async (_event, durationSeconds: number) => {
  return await saveClipAndRecord(durationSeconds);
});
ipcMain.handle("settings:get", () => readSettings());
ipcMain.handle("settings:save", (_event, settings: ClipSettings) => saveSettings(settings));
ipcMain.handle("library:list", () => readClips());
ipcMain.handle("library:rename", (_event, id: string, newTitle: string) => {
  const clips = readClips();
  const clip = clips.find((c) => c.id === id);
  if (clip) {
    clip.title = newTitle;
    
    // Rename file on disk
    if (existsSync(clip.filePath)) {
      const safeTitle = newTitle.replace(/[\\/:*?"<>|]/g, "_").trim() || "clip";
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
ipcMain.handle("library:clipThumbnailUrl", (_event, filePath: string) => clipThumbnailUrl(filePath));
ipcMain.handle("library:clipPlaybackUrl", (_event, filePath: string, audioTracks: string[]) => clipPlaybackUrl(filePath, audioTracks));
ipcMain.handle("processes:list", () => listActiveProcesses());
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
