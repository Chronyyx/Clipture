export type EncoderName = "NVENC" | "Media Foundation Hardware" | "Software" | "Unavailable";
export type ThemeFontId = "glitten" | "milate";

export interface FrameDropReasonDeltas {
  reportedDroppedFrames: number;
  captureQueueOverflow: number;
  captureSlotExhaustion: number;
  schedulerDeadlineMisses: number;
  encoderBackpressure: number;
  encoderBackpressureOther: number;
  captureCallbackErrors: number;
  captureQueueCoalesced: number;
  sourceFramesSuperseded: number;
  captureSamplerRejections: number;
  captureNonMonotonicTimestamps: number;
  captureAcquireTimeouts: number;
  captureAccessLosses: number;
  captureFallbacks: number;
  schedulerRepeats: number;
  encoderQueueDrops: number;
  encoderRepeatsCoalesced: number;
  nvencSurfaceDrops: number;
  nvencInputDrops: number;
}

export interface FrameDropActivityDeltas {
  captureUpdatesAcquired: number;
  desktopPresents: number;
  pointerUpdates: number;
  freshFramesPublished: number;
  accumulatedSourceFrames: number;
  accumulationEvents: number;
  captureAcquireImmediateMisses: number;
  captureAcquireGraceHits: number;
  captureAcquireGraceTimeouts: number;
  captureClockTickRequests: number;
  captureClockTickWakeups: number;
  captureClockTickCoalesced: number;
  captureClockTickCompletions: number;
  captureClockTickCompletionWaits: number;
  captureClockTickCompletionTimeouts: number;
  presentLatchWaits: number;
  presentLatchHits: number;
  presentLatchTimeouts: number;
  catchUpEvents: number;
  historicalFramesRecovered: number;
  catchUpRepeatedTicks: number;
  encoderAdmissionRejections: number;
  encoderFramesAccepted: number;
  encoderPacketsProduced: number;
  encoderDistinctSourceFrames: number;
  encoderRepeatedSourceFrames: number;
  encoderUnknownSourceFrames: number;
  replayPacketsPersisted: number;
}

export interface FrameDropTimelineSample {
  sampledAt: string;
  windowMs: number;
  reset: boolean;
  captureEpochChanged: boolean;
  dominantCountedReason: string;
  visualFreshnessBottleneck: string;
  countedReasonTotal: number;
  accountingDifference: number;
  reportedDropsPerSecond: number;
  freshnessRates: {
    targetFps: number;
    captureClockRequests: number;
    captureClockWakeups: number;
    captureClockCompletionTimeouts: number;
    presentLatchWaits: number;
    presentLatchHits: number;
    presentLatchTimeouts: number;
    captureAcquireImmediateMisses: number;
    captureAcquireGraceHits: number;
    captureAcquireGraceTimeouts: number;
    desktopPresents: number;
    desktopUpdateSupply: number;
    captureUpdates: number;
    freshFramesPublished: number;
    encoderFramesAccepted: number;
    encoderPacketsProduced: number;
    distinctSourceFramesEncoded: number;
  };
  reasons: FrameDropReasonDeltas;
  activity: FrameDropActivityDeltas;
  context: {
    captureEpoch: number;
    capturePressure: EngineDiagnostics["capturePressure"];
    activeCaptureBackend: string;
    captureClockMode: string;
    desktopPresentFps: number;
    publishedFreshFps: number;
    recentCaptureFps: number;
    recentEncoderInputFps: number;
    recentEncoderOutputFps: number;
    recentEncoderDistinctSourceFps: number;
    stillFrameDuplicationEnabled: boolean;
    captureSourceIntervalP95_100ns: number;
    publishedWallIntervalP95_100ns: number;
    schedulerWakeLatenessP95_100ns: number;
    encoderQueueResidenceP95_100ns: number;
    encoderInputIntervalP95_100ns: number;
    encoderOutputIntervalP95_100ns: number;
    queuedFrames: number;
    encoderQueuedFreshFrames: number;
    encoderQueuedRepeatFrames: number;
    nvencInFlightFrames: number;
    captureAcquireP95_100ns: number;
    capturePreparationP95_100ns: number;
    captureProcessingP95_100ns: number;
    inputPreparationP95_100ns: number;
    inputMapP95_100ns: number;
    nvencCallP95_100ns: number;
    outputEventWaitP95_100ns: number;
    outputLockP95_100ns: number;
    outputCopyP95_100ns: number;
    outputUnmapP95_100ns: number;
    maximumCaptureGap100ns: number;
    maximumSubmitLatency100ns: number;
    replayArchiveQueuedBytes: number;
    replayArchiveQueuedPackets: number;
    replayArchiveWriteFailures: number;
  };
}

export interface FrameDropAnalysis {
  sampleIntervalMinimumMs: number;
  maximumRetainedSamples: number;
  retainedWindowMs: number;
  definitions: {
    reportedDroppedFrames: string;
    countedReasons: string[];
    supportingIndicators: string[];
  };
  totals: FrameDropReasonDeltas;
  latest: FrameDropTimelineSample | null;
  timeline: FrameDropTimelineSample[];
}

export interface VideoCadenceBucket {
  secondIndex: number;
  duration100ns: number;
  sampleCount: number;
  distinctSourceFrames: number;
  repeatedSourceFrames: number;
  unknownSourceFrames: number;
  desktopPresentSourceFrames: number;
  pointerOnlySourceFrames: number;
  unknownUpdateKindSourceFrames: number;
  maximumSampleGap100ns: number;
  distinctSourceFps: number;
  desktopPresentSourceFps: number;
}

export interface VideoCadenceAnalysis {
  available: boolean;
  targetFps: number;
  span100ns: number;
  targetFrameDuration100ns: number;
  maximumSampleGap100ns: number;
  expectedOutputTicks: number;
  sampleCount: number;
  distinctSourceFrames: number;
  repeatedSourceFrames: number;
  unknownSourceFrames: number;
  desktopPresentSourceFrames: number;
  pointerOnlySourceFrames: number;
  unknownUpdateKindSourceFrames: number;
  longestHeldRunSamples: number;
  gapEvents: number;
  missingFrameSlots: number;
  underTargetSeconds: number;
  underTargetDesktopPresentSeconds: number;
  distinctSourceFps: number;
  desktopPresentSourceFps: number;
  repeatRatio: number;
  worstSecondDistinctSourceFps: number;
  worstSecondDesktopPresentSourceFps: number;
  buckets: VideoCadenceBucket[];
}

export interface EngineDiagnostics {
  captureApi: string;
  requestedCaptureBackend: string;
  activeCaptureBackend: string;
  captureFallbackReason: string;
  captureTargetKind: string;
  captureTargetName: string;
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
  captureAcquireImmediateMisses: number;
  captureAcquireGraceHits: number;
  captureAcquireGraceTimeouts: number;
  captureAccessLosses: number;
  captureRecreationAttempts: number;
  captureRecreationSuccesses: number;
  captureFallbacks: number;
  captureClockMode: string;
  captureClockTickRequests: number;
  captureClockTickWakeups: number;
  captureClockTickCoalesced: number;
  captureClockTickCompletions: number;
  captureClockTickCompletionWaits: number;
  captureClockTickCompletionTimeouts: number;
  presentLatchWaits: number;
  presentLatchHits: number;
  presentLatchTimeouts: number;
  catchUpEvents: number;
  historicalFramesRecovered: number;
  catchUpRepeatedTicks: number;
  desktopPresentFps: number;
  publishedFreshFps: number;
  recentPublishedFreshFps: number;
  recentEncoderInputFps: number;
  recentEncoderOutputFps: number;
  recentEncoderDistinctSourceFps: number;
  encodedRepeatRatio: number;
  encodedMotionRepeatRatioPercent: number;
  recentMotionRepeatRatioPercent: number;
  motionFramesTotal: number;
  motionFramesRepeated: number;
  recentMotionFramesTotal: number;
  recentMotionFramesRepeated: number;
  stillFrameDuplicationEnabled: boolean;
  encoderDistinctSourceFrames: number;
  encoderRepeatedSourceFrames: number;
  encoderUnknownSourceFrames: number;
  recentEncoderRepeatedSourceFrames: number;
  recentEncoderUnknownSourceFrames: number;
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
  frameQueueMaxDepth: number;
  sourceFramesSuperseded: number;
  captureSlotDrops: number;
  captureCallbackErrors: number;
  schedulerDroppedFrames: number;
  schedulerRepeatedFrames: number;
  encoderQueueDrops: number;
  encoderRepeatCoalesced: number;
  encoderAdmissionRejections: number;
  encoderQueuedFreshFrames: number;
  encoderQueuedRepeatFrames: number;
  nvencSurfaceDrops: number;
  nvencInputDrops: number;
  encoderBackpressureDrops: number;
  nvencInFlightFrames: number;
  recentDropWindowMs: number;
  recentDroppedFrames: number;
  recentCaptureOverflowDrops: number;
  recentCaptureSlotDrops: number;
  recentSchedulerDroppedFrames: number;
  recentEncoderBackpressureDrops: number;
  recentEncoderBackpressureOtherDrops: number;
  recentCaptureCallbackErrors: number;
  recentCaptureCoalescedDrops: number;
  recentSourceFramesSuperseded: number;
  recentCaptureSamplerRejections: number;
  recentCaptureNonMonotonicTimestamps: number;
  recentCaptureAcquireTimeouts: number;
  recentCaptureAccessLosses: number;
  recentCaptureFallbacks: number;
  recentSchedulerRepeatedFrames: number;
  recentEncoderQueueDrops: number;
  recentEncoderRepeatCoalesced: number;
  recentCatchUpEvents: number;
  recentHistoricalFramesRecovered: number;
  recentCatchUpRepeatedTicks: number;
  recentEncoderAdmissionRejections: number;
  recentNvencSurfaceDrops: number;
  recentNvencInputDrops: number;
  recentDropDominantReason: string;
  recentVisualFreshnessBottleneck: string;
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
  recentCaptureSourceIntervalP50_100ns: number;
  recentCaptureSourceIntervalP95_100ns: number;
  recentCaptureSourceIntervalMaximum100ns: number;
  recentPublishedPtsIntervalP50_100ns: number;
  recentPublishedPtsIntervalP95_100ns: number;
  recentPublishedPtsIntervalMaximum100ns: number;
  recentPublishedWallIntervalP50_100ns: number;
  recentPublishedWallIntervalP95_100ns: number;
  recentPublishedWallIntervalMaximum100ns: number;
  recentCapturePreparationP50_100ns: number;
  recentCapturePreparationP95_100ns: number;
  recentCaptureCursorP95_100ns: number;
  recentCaptureProcessingP50_100ns: number;
  recentCaptureProcessingP95_100ns: number;
  recentSchedulerWakeLatenessP50_100ns: number;
  recentSchedulerWakeLatenessP95_100ns: number;
  recentSchedulerWakeLatenessMaximum100ns: number;
  recentEncoderQueueResidenceP50_100ns: number;
  recentEncoderQueueResidenceP95_100ns: number;
  recentEncoderQueueResidenceMaximum100ns: number;
  recentEncoderInputIntervalP50_100ns: number;
  recentEncoderInputIntervalP95_100ns: number;
  recentEncoderInputIntervalMaximum100ns: number;
  recentEncoderOutputIntervalP50_100ns: number;
  recentEncoderOutputIntervalP95_100ns: number;
  recentEncoderOutputIntervalMaximum100ns: number;
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
  replayArchiveResidentBytes: number;
  replayArchiveResidentBudgetBytes: number;
  replayArchiveReadCacheBytes: number;
  replayArchiveResidentPackets: number;
  replayArchiveDiskBackedPackets: number;
  replayArchiveQueuedBytes: number;
  replayArchivePersistedPackets: number;
  replayArchiveSpillCandidateInspections: number;
  replayArchiveWriteFailures: number;
  replayArchiveQueuedPackets: number;
  replayArchiveSegments: number;
  replayArchiveMaximumWriteBytes: number;
  pcmRecoveryActive: boolean;
  capturedFrames: number;
  queuedFrames: number;
  encoderAcceptedFrames: number;
  encoderOutputPackets: number;
  audioCapturedPackets: number;
  bufferDurationSeconds: number;
  lastClipCadence: VideoCadenceAnalysis;
  degraded: boolean;
  status: string;
}

export interface ClipSettings {
  uiTheme: "graphite" | "light" | ThemeFontId | "custom";
  customMainColor: string;
  customAccentColor: string;
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

export interface SaveIoTimelineBucket {
  startMs: number;
  replayReadBytes: number;
  outputWriteBytes: number;
  replayReadBusyUs: number;
  outputWriteBusyUs: number;
  pacingWaitUs: number;
  replayReadCalls: number;
  outputWriteCalls: number;
  pacingWaitCalls: number;
  maximumReplayReadUs: number;
  maximumOutputWriteUs: number;
  maximumPacingWaitUs: number;
  pressureLevel: number;
  frameQueueDepth: number;
  encoderQueueDepth: number;
  nvencInFlight: number;
  droppedFramesDelta: number;
  captureGap100ns: number;
  capturePublicationAge100ns: number;
}

export interface SaveIoSlowOperation {
  operation: string;
  startMs: number;
  durationUs: number;
  bytes: number;
}

export interface SaveIoAnalysis {
  bucketMs: number;
  elapsedMs: number;
  timelineTruncated: boolean;
  omittedSlowOperations: number;
  storageSeekPenalty: string;
  ioPriority: string;
  lowIoPriorityApplied: boolean;
  preallocated: boolean;
  finalFileBytes: number;
  diskBackedSourceBytes: number;
  processReadOperationsDelta: number;
  processWriteOperationsDelta: number;
  processReadBytesDelta: number;
  processWriteBytesDelta: number;
  processOtherBytesDelta: number;
  timeline: SaveIoTimelineBucket[];
  slowOperations: SaveIoSlowOperation[];
}

export interface SaveIoAnalyzerState {
  available: boolean;
  armed: boolean;
  traceReady: boolean;
  capturedAt?: string;
}

export interface SaveClipResult {
  ok: boolean;
  message: string;
  clip?: ClipRecord;
  saveIoAnalysis?: SaveIoAnalysis[];
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
  getSaveIoAnalyzerState(): Promise<SaveIoAnalyzerState>;
  setSaveIoAnalyzerArmed(armed: boolean): Promise<SaveIoAnalyzerState>;
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
  openThemeFontDownload(theme: ThemeFontId): Promise<void>;
  onLibraryChanged: (callback: (addedClip?: ClipRecord) => void) => () => void;
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
