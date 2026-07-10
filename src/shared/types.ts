export type EncoderName = "NVENC" | "Media Foundation Hardware" | "Software" | "Unavailable";

export interface EngineDiagnostics {
  captureApi: string;
  activeEncoder: EncoderName;
  encoderMode: string;
  gpu: string;
  microphoneDevice: string;
  display: string;
  hdrTonemapping: boolean;
  videoSourceResolution: string;
  videoOutputResolution: string;
  videoScaling: string;
  clipTargetResolution: string;
  codec: string;
  resolution: string;
  fps: number;
  bitrateMbps: number;
  hardwareAcceleration: boolean;
  droppedFrames: number;
  nvencAvailable: boolean;
  engineRunning: boolean;
  d3d11Ready: boolean;
  captureReady: boolean;
  audioReady: boolean;
  muxReady: boolean;
  bufferedVideoPackets: number;
  bufferedAudioPackets: number;
  capturedFrames: number;
  queuedFrames: number;
  encoderAcceptedFrames: number;
  encoderOutputPackets: number;
  audioCapturedPackets: number;
  bufferDurationSeconds: number;
  degraded: boolean;
  status: string;
}

export interface ClipSettings {
  clipLengthSeconds: number;
  fps: 24 | 30 | 60;
  bitrateMbps: number;
  autoBitrate: boolean;
  maxAutoBitrateMbps: number;
  nvencPreset: 1 | 2 | 3 | 4 | 5;
  resolutionPreset: "system" | "144p" | "360p" | "720p" | "1080p" | "1440p" | "4k";
  monitorMode: "primary";
  monitorId: string;
  startOnLogin: boolean;
  hotkey: string;
  clipSound: string;
  showNotification: boolean;
  notificationPosition: "top-right" | "top-left" | "bottom-right" | "bottom-left" | "top-center";
  saveFolder: string;
  audioSources: AudioSourceRule[];
}

export interface AudioSourceRule {
  id: string;
  label: string;
  kind: "microphone" | "game" | "app" | "rest" | "mix" | "system";
  processName?: string;
  processNames?: string[];
  captureAllSystem?: boolean;
  enabled: boolean;
  omitIfSilent: boolean;
  volume?: number;
  voiceIsolation?: boolean;
  voiceIsolationWeight?: number;
  noiseGateEnabled?: boolean;
  autoNoiseGate?: boolean;
  noiseGateThreshold?: number;
  noiseGateDebounceMs?: number;
  micDeviceId?: string;
  micDeviceMatchKey?: string;
  micDeviceName?: string;
}

export interface AudioInputDevice {
  id: string;
  name: string;
  isDefault: boolean;
  state?: "active" | "unavailable";
  matchKey?: string;
}

export interface ClipSoundOption {
  id: string;
  label: string;
  url?: string;
  builtIn: boolean;
}

export interface DisplayDevice {
  id: string;
  name: string;
  isPrimary: boolean;
  width: number;
  height: number;
  x: number;
  y: number;
  hdr: boolean;
}

export interface ClipRecord {
  id: string;
  title: string;
  gameOrApp: string;
  isGame?: boolean;
  createdAt: string;
  durationSeconds: number;
  filePath: string;
  resolution: string;
  recommendedResolution?: string;
  segmentFiles?: string[];
  segmentResolutions?: string[];
  fps: number;
  encoder: string;
  audioTracks: string[];
  focusedApps?: string[];
}

export interface SaveClipResult {
  ok: boolean;
  message: string;
  clip?: ClipRecord;
}

export type UpdateStatus = "idle" | "checking" | "available" | "downloading" | "ready" | "error";

export interface UpdateState {
  status: UpdateStatus;
  version?: string;
  message?: string;
  checkedAt?: string;
}

export interface ActiveProcess {
  name: string;
  pid: number;
}

export interface CliptureApi {
  getDiagnostics(): Promise<EngineDiagnostics>;
  getSettings(): Promise<ClipSettings>;
  saveSettings(settings: ClipSettings): Promise<ClipSettings>;
  saveClip(durationSeconds: number): Promise<SaveClipResult>;
  listClips(): Promise<ClipRecord[]>;
  clipUrl(filePath: string): Promise<string>;
  clipThumbnailUrl(filePath: string): Promise<string>;
  clipPlaybackUrl(filePath: string, audioTracks: string[]): Promise<{ url: string; mixed: boolean; message: string }>;
  listActiveProcesses(): Promise<ActiveProcess[]>;
  listAudioInputDevices(): Promise<AudioInputDevice[]>;
  listDisplayDevices(): Promise<DisplayDevice[]>;
  listClipSounds(): Promise<ClipSoundOption[]>;
  importClipSound(): Promise<ClipSoundOption | undefined>;
  revealSoundsFolder: () => Promise<void>;
  revealClip: (filePath: string) => Promise<void>;
  renameClip: (id: string, newTitle: string) => Promise<boolean>;
  getUpdateState(): Promise<UpdateState>;
  checkForUpdates(): Promise<UpdateState>;
  downloadUpdate(): Promise<void>;
  installUpdate(): Promise<void>;
  onLibraryChanged: (callback: () => void) => () => void;
  onUpdateStateChanged: (callback: (state: UpdateState) => void) => () => void;
  onPlaySound: (callback: (sound: string) => void) => () => void;
  onShowNotification: (callback: (thumbnailUrl: string, position: string, message?: string) => void) => () => void;
  hideNotification: () => void;
  selectFolder(currentPath: string): Promise<string | undefined>;
}

declare global {
  interface Window {
    clipture: CliptureApi;
  }
}
