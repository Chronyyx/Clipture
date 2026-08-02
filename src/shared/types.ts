export type EncoderName = "NVENC" | "Media Foundation Hardware" | "Software" | "Unavailable";

export interface EngineDiagnostics {
  captureApi: string;
  requestedCaptureBackend: string;
  activeCaptureBackend: string;
  captureFallbackReason: string;
  displayRefreshNumerator: number;
  displayRefreshDenominator: number;
  displayRefreshHz: number;
  captureAcquiredUpdates: number;
  captureDesktopPresents: number;
  capturePointerUpdates: number;
  capturePublishedFrames: number;
  captureAccumulatedFrames: number;
  captureAccumulationEvents: number;
  captureSamplerRejections: number;
  captureNonMonotonicTimestamps: number;
  captureAcquireTimeouts: number;
  captureAccessLosses: number;
  captureRecreationAttempts: number;
  captureRecreationSuccesses: number;
  captureFallbacks: number;
  desktopPresentFps: number;
  publishedFreshFps: number;
  recentPublishedFreshFps: number;
  recentEncoderInputFps: number;
  recentEncoderOutputFps: number;
  encodedRepeatRatio: number;
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
  captureOverflowDrops: number;
  captureCoalescedDrops: number;
  sourceFramesSuperseded: number;
  captureSlotDrops: number;
  captureCallbackErrors: number;
  schedulerDroppedFrames: number;
  schedulerRepeatedFrames: number;
  encoderQueueDrops: number;
  encoderRepeatCoalesced: number;
  nvencSurfaceDrops: number;
  nvencInputDrops: number;
  encoderBackpressureDrops: number;
  nvencInFlightFrames: number;
  maximumCaptureGap100ns: number;
  maximumSubmitLatency100ns: number;
  averageScaleLatency100ns: number;
  maximumScaleLatency100ns: number;
  averageInputMapLatency100ns: number;
  maximumInputMapLatency100ns: number;
  averageNvencCallLatency100ns: number;
  maximumNvencCallLatency100ns: number;
  averageOutputDrainLatency100ns: number;
  maximumOutputDrainLatency100ns: number;
  recentCaptureAcquireP95_100ns: number;
  recentCapturePreparationP50_100ns: number;
  recentCapturePreparationP95_100ns: number;
  recentCaptureCursorP95_100ns: number;
  recentCaptureProcessingP50_100ns: number;
  recentCaptureProcessingP95_100ns: number;
  recentInputPreparationP50_100ns: number;
  recentInputPreparationP95_100ns: number;
  recentInputMapP50_100ns: number;
  recentInputMapP95_100ns: number;
  recentNvencCallP50_100ns: number;
  recentNvencCallP95_100ns: number;
  recentOutputEventWaitP50_100ns: number;
  recentOutputEventWaitP95_100ns: number;
  recentOutputLockP95_100ns: number;
  recentOutputCopyP95_100ns: number;
  recentOutputUnmapP95_100ns: number;
  nvencZeroCopyFrames: number;
  nvencCopyFallbackFrames: number;
  nvencConvertedFrames: number;
  captureEpoch: number;
  capturePressure: "healthy" | "elevated" | "critical";
  nvencAvailable: boolean;
  engineRunning: boolean;
  d3d11Ready: boolean;
  captureReady: boolean;
  audioReady: boolean;
  muxReady: boolean;
  bufferedVideoPackets: number;
  bufferedAudioPackets: number;
  videoReplayArchiveHealthy: boolean;
  audioReplayArchiveHealthy: boolean;
  replayArchiveDiskBytes: number;
  replayArchiveRamFallbackBytes: number;
  replayArchiveQueuedBytes: number;
  replayArchivePersistedPackets: number;
  replayArchiveWriteFailures: number;
  replayArchiveQueuedPackets: number;
  replayArchiveSegments: number;
  replayArchiveMaximumWriteBytes: number;
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
  importedVideoDirectories?: string[];
  importedVideoTitles?: Record<string, string>;
  audioSources: AudioSourceRule[];
}

export interface AudioSourceRule {
  id: string;
  label: string;
  kind: "microphone" | "game" | "app" | "rest" | "mix" | "system";
  processName?: string;
  processNames?: string[];
  executablePath?: string;
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
  librarySource?: "clip" | "imported";
  folderName?: string;
  importedRoot?: string;
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
  executablePath?: string;
}

export interface CliptureApi {
  getDiagnostics(): Promise<EngineDiagnostics>;
  exportDiagnostics(): Promise<string | undefined>;
  getSettings(): Promise<ClipSettings>;
  saveSettings(settings: ClipSettings): Promise<ClipSettings>;
  saveClip(durationSeconds: number): Promise<SaveClipResult>;
  listClips(): Promise<ClipRecord[]>;
  deleteClips(ids: string[]): Promise<boolean>;
  importVideoFolders(): Promise<boolean>;
  clipUrl(filePath: string): Promise<string>;
  clipIconUrl(clip: ClipRecord, preferredLabels?: string[]): Promise<string>;
  processIconUrl(processName: string, executablePath?: string): Promise<string>;
  clipThumbnailUrl(filePath: string): Promise<string>;
  clipPlaybackUrl(filePath: string, audioTracks: string[]): Promise<{ url: string; mixed: boolean; message: string; audioChunkUrl?: string; audioChunkSeconds?: number }>;
  releasePlaybackCache(): Promise<boolean>;
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
