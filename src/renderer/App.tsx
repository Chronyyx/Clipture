import { Activity, Check, ChevronDown, ChevronLeft, ChevronRight, Clapperboard, Clock, Download, Edit3, ExternalLink, Feather, FolderOpen, Gamepad2, Leaf, Library, Maximize2, Mic, Minus, Moon, Paintbrush, Palette, Pause, Play, Plus, RefreshCw, Save, Search, SlidersHorizontal, Sun, Trash2, Upload, Volume2, X } from "lucide-react";
import { Fragment, useCallback, useEffect, useMemo, useRef, useState } from "react";
// @ts-ignore
import logoUrl from "../../assets/svgviewer-output.svg";
import type { CSSProperties, KeyboardEvent } from "react";
import type { ActiveProcess, AudioInputDevice, ClipRecord, ClipSettings, DisplayDevice, EngineDiagnostics, AudioSourceRule, ClipSoundOption, SaveIoAnalyzerState, ThemeFontId, UpdateState } from "../shared/types";
import { applyUiTheme, cacheAndApplyUiTheme, normalizeThemeColor, refreshLocalThemeFont } from "./theme";

type Tab = "library" | "settings" | "diagnostics";

type AppNotice = {
  message: string;
  tab?: Tab;
  durationMs: number;
};

const defaultDiagnostics: EngineDiagnostics = {
  captureApi: "Windows.Graphics.Capture",
  requestedCaptureBackend: "auto",
  activeCaptureBackend: "none",
  captureFallbackReason: "",
  displayRefreshNumerator: 0,
  displayRefreshDenominator: 1,
  displayRefreshHz: 0,
  captureAcquiredUpdates: 0,
  captureDesktopPresents: 0,
  capturePointerUpdates: 0,
  capturePublishedFrames: 0,
  captureAccumulatedFrames: 0,
  captureAccumulationEvents: 0,
  captureSamplerRejections: 0,
  captureNonMonotonicTimestamps: 0,
  captureAcquireTimeouts: 0,
  captureAccessLosses: 0,
  captureRecreationAttempts: 0,
  captureRecreationSuccesses: 0,
  captureFallbacks: 0,
  desktopPresentFps: 0,
  publishedFreshFps: 0,
  recentPublishedFreshFps: 0,
  recentEncoderInputFps: 0,
  recentEncoderOutputFps: 0,
  encodedRepeatRatio: 0,
  activeEncoder: "Unavailable",
  encoderMode: "Unavailable",
  gpu: "Loading",
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
    captureOverflowDrops: 0,
    captureCoalescedDrops: 0,
    sourceFramesSuperseded: 0,
    captureSlotDrops: 0,
    captureCallbackErrors: 0,
    schedulerDroppedFrames: 0,
    schedulerRepeatedFrames: 0,
    encoderQueueDrops: 0,
    encoderRepeatCoalesced: 0,
    encoderQueuedFreshFrames: 0,
    encoderQueuedRepeatFrames: 0,
    nvencSurfaceDrops: 0,
    nvencInputDrops: 0,
    encoderBackpressureDrops: 0,
    nvencInFlightFrames: 0,
  maximumCaptureGap100ns: 0,
  maximumSubmitLatency100ns: 0,
  averageScaleLatency100ns: 0,
  maximumScaleLatency100ns: 0,
  averageInputMapLatency100ns: 0,
  maximumInputMapLatency100ns: 0,
  averageNvencCallLatency100ns: 0,
  maximumNvencCallLatency100ns: 0,
  averageOutputDrainLatency100ns: 0,
  maximumOutputDrainLatency100ns: 0,
  recentCaptureAcquireP95_100ns: 0,
  recentCapturePreparationP50_100ns: 0,
  recentCapturePreparationP95_100ns: 0,
  recentCaptureCursorP95_100ns: 0,
  recentCaptureProcessingP50_100ns: 0,
  recentCaptureProcessingP95_100ns: 0,
  recentInputPreparationP50_100ns: 0,
  recentInputPreparationP95_100ns: 0,
  recentInputMapP50_100ns: 0,
  recentInputMapP95_100ns: 0,
  recentNvencCallP50_100ns: 0,
  recentNvencCallP95_100ns: 0,
  recentOutputEventWaitP50_100ns: 0,
  recentOutputEventWaitP95_100ns: 0,
  recentOutputLockP95_100ns: 0,
  recentOutputCopyP95_100ns: 0,
  recentOutputUnmapP95_100ns: 0,
  nvencZeroCopyFrames: 0,
  nvencCopyFallbackFrames: 0,
  nvencConvertedFrames: 0,
    captureEpoch: 0,
    capturePressure: "healthy",
    nvencAvailable: false,
    engineRunning: false,
    d3d11Ready: false,
    captureReady: false,
    audioReady: false,
    muxReady: false,
    bufferedVideoPackets: 0,
    bufferedAudioPackets: 0,
    videoReplayArchiveHealthy: false,
    audioReplayArchiveHealthy: false,
    replayArchiveDiskBytes: 0,
    replayArchiveRamFallbackBytes: 0,
    replayArchiveResidentBytes: 0,
    replayArchiveResidentBudgetBytes: 0,
    replayArchiveReadCacheBytes: 0,
    replayArchiveResidentPackets: 0,
    replayArchiveDiskBackedPackets: 0,
    replayArchiveQueuedBytes: 0,
    replayArchivePersistedPackets: 0,
    replayArchiveSpillCandidateInspections: 0,
    replayArchiveWriteFailures: 0,
    replayArchiveQueuedPackets: 0,
    replayArchiveSegments: 0,
    replayArchiveMaximumWriteBytes: 0,
    pcmRecoveryActive: false,
    capturedFrames: 0,
    queuedFrames: 0,
    encoderAcceptedFrames: 0,
    encoderOutputPackets: 0,
    audioCapturedPackets: 0,
    bufferDurationSeconds: 0,
    degraded: true,
    status: "Connecting to native engine"
};

// @ts-ignore
import { RNNoiseNode } from 'simple-rnnoise-wasm';

// @ts-ignore
import workletUrl from 'simple-rnnoise-wasm/rnnoise.worklet.js?url';
// @ts-ignore
import wasmUrl from 'simple-rnnoise-wasm/rnnoise.wasm?url';

type GateMeterSample = {
  level: number;
  open: boolean;
};

const emptyGateMeterSamples = Array.from({ length: 36 }, () => ({ level: 0, open: false }));

const defaultUpdateState: UpdateState = { status: "idle" };

function updateButtonTitle(updateState: UpdateState): string {
  switch (updateState.status) {
    case "checking":
      return "Checking for updates";
    case "downloading":
      return updateState.message || "Downloading update";
    case "ready":
      return "Restart to install update";
    case "error":
      return updateState.message ? `Update check failed: ${updateState.message}` : "Update check failed";
    default:
      return "Check for updates";
  }
}

function TitlebarUpdateControls({
  updateState,
  onCheck,
  onDownload,
  onInstall
}: {
  updateState: UpdateState;
  onCheck: () => void;
  onDownload: () => void;
  onInstall: () => void;
}) {
  const [forceSpin, setForceSpin] = useState(false);

  const handleCheck = () => {
    setForceSpin(true);
    setTimeout(() => setForceSpin(false), 1000);
    onCheck();
  };

  const checking = updateState.status === "checking" || forceSpin;
  const downloading = updateState.status === "downloading";
  const ready = updateState.status === "ready";
  const error = updateState.status === "error";
  const detected = updateState.status === "available" || downloading || ready;
  
  const refreshTitle = error 
    ? (updateState.message ? `Update failed: ${updateState.message}` : "Update check failed")
    : (checking ? "Checking for updates" : "Check for updates");

  return (
    <div className="titlebar-update-controls">
      <button
        className={`titlebar-update-button refresh ${checking ? "checking" : ""} ${error && !checking ? "error" : ""}`}
        title={refreshTitle}
        aria-label={refreshTitle}
        disabled={checking || downloading}
        onClick={handleCheck}
      >
        <RefreshCw size={14} strokeWidth={2.1} />
      </button>
      {detected && (
        <button
          className={`titlebar-update-button download ${updateState.status}`}
          title={updateButtonTitle(updateState)}
          aria-label={updateButtonTitle(updateState)}
          disabled={downloading}
          onClick={ready ? onInstall : (updateState.status === "available" ? onDownload : undefined)}
        >
          <Download size={16} strokeWidth={2.1} />
        </button>
      )}
    </div>
  );
}

function TestMicButton({
  volume,
  voiceIsolation,
  voiceIsolationWeight,
  noiseGateEnabled,
  autoNoiseGate,
  noiseGateThreshold,
  noiseGateDebounceMs
}: {
  volume: number;
  voiceIsolation: boolean;
  voiceIsolationWeight: number;
  noiseGateEnabled: boolean;
  autoNoiseGate: boolean;
  noiseGateThreshold: number;
  noiseGateDebounceMs: number;
}) {
  const [testing, setTesting] = useState(false);
  const [meterSamples, setMeterSamples] = useState<GateMeterSample[]>(emptyGateMeterSamples);
  const audioContextRef = useRef<AudioContext | null>(null);
  const streamRef = useRef<MediaStream | null>(null);
  const wetGainRef = useRef<GainNode | null>(null);
  const dryGainRef = useRef<GainNode | null>(null);
  const masterGainRef = useRef<GainNode | null>(null);
  const gateGainRef = useRef<GainNode | null>(null);
  const gateConfigRef = useRef({ noiseGateEnabled, autoNoiseGate, noiseGateThreshold, noiseGateDebounceMs });

  useEffect(() => {
    gateConfigRef.current = { noiseGateEnabled, autoNoiseGate, noiseGateThreshold, noiseGateDebounceMs };
  }, [noiseGateEnabled, autoNoiseGate, noiseGateThreshold, noiseGateDebounceMs]);

  useEffect(() => {
    if (!testing) {
      setMeterSamples(emptyGateMeterSamples);
      if (streamRef.current) {
        streamRef.current.getTracks().forEach(t => t.stop());
        streamRef.current = null;
      }
      if (audioContextRef.current) {
        audioContextRef.current.close().catch(console.error);
        audioContextRef.current = null;
      }
      return;
    }

    let isCancelled = false;

    navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false } })
      .then(async (stream) => {
        if (isCancelled) {
          stream.getTracks().forEach(t => t.stop());
          return;
        }

        streamRef.current = stream;
        const ctx = new AudioContext();
        audioContextRef.current = ctx;

        const source = ctx.createMediaStreamSource(stream);
        masterGainRef.current = ctx.createGain();
        masterGainRef.current.connect(ctx.destination);
        masterGainRef.current.gain.value = volume;
        
        gateGainRef.current = ctx.createGain();
        gateGainRef.current.connect(masterGainRef.current);
        
        const analyser = ctx.createAnalyser();
        analyser.fftSize = 512;
        source.connect(analyser);
        
        const pcmData = new Float32Array(analyser.fftSize);
        let lastGateOpen = false;
        let lastVoiceMs = performance.now();
        let lastMeterUpdateMs = 0;
        
        const checkGate = () => {
          if (isCancelled) return;
          requestAnimationFrame(checkGate);
          
          analyser.getFloatTimeDomainData(pcmData);
          let sum = 0;
          for (let i = 0; i < pcmData.length; i++) sum += pcmData[i] * pcmData[i];
          const rms = Math.sqrt(sum / pcmData.length);
          
          const { noiseGateEnabled, autoNoiseGate, noiseGateThreshold, noiseGateDebounceMs } = gateConfigRef.current;
          const threshold = autoNoiseGate ? 0.01 : noiseGateThreshold;
          const now = performance.now();
          const crossedThreshold = rms > threshold;
          if (crossedThreshold) lastVoiceMs = now;
          
          const gateOpen = !noiseGateEnabled || crossedThreshold || now - lastVoiceMs <= noiseGateDebounceMs;
          if (gateOpen !== lastGateOpen && gateGainRef.current) {
            lastGateOpen = gateOpen;
            gateGainRef.current.gain.setTargetAtTime(gateOpen ? 1.0 : 0.0, ctx.currentTime, gateOpen ? 0.015 : 0.05);
          }

          if (now - lastMeterUpdateMs > 45) {
            lastMeterUpdateMs = now;
            const level = visualizerLevelFromRms(rms);
            setMeterSamples((samples) => [...samples.slice(1), { level, open: gateOpen }]);
          }
        };
        requestAnimationFrame(checkGate);

        if (voiceIsolation) {
          try {
            const wasmPromise = fetch(wasmUrl).then(r => r.arrayBuffer()).then(buf => WebAssembly.compile(buf));
            await RNNoiseNode.register(ctx, [workletUrl, wasmPromise]);
            if (isCancelled) return;
            
            const rnnoise = new RNNoiseNode(ctx);
            
            // RNNoise has its own VAD logic we could theoretically hook into, but RMS is fine for UI testing
            rnnoise.onstatus = (e: any) => {
              if (gateConfigRef.current.noiseGateEnabled && gateConfigRef.current.autoNoiseGate) {
                const vadProb = (e as any).data?.vad ?? (typeof (e as any).data === 'number' ? (e as any).data : 0.0);
                const now = performance.now();
                if (vadProb > 0.5) lastVoiceMs = now;
                const gateOpen = vadProb > 0.5 || now - lastVoiceMs <= gateConfigRef.current.noiseGateDebounceMs;
                if (gateOpen !== lastGateOpen && gateGainRef.current) {
                  lastGateOpen = gateOpen;
                  gateGainRef.current.gain.setTargetAtTime(gateOpen ? 1.0 : 0.0, ctx.currentTime, gateOpen ? 0.015 : 0.05);
                }
              }
            };
            
            wetGainRef.current = ctx.createGain();
            dryGainRef.current = ctx.createGain();
            
            source.connect(rnnoise);
            rnnoise.connect(wetGainRef.current);
            source.connect(dryGainRef.current);
            
            wetGainRef.current.connect(gateGainRef.current);
            dryGainRef.current.connect(gateGainRef.current);
            
            wetGainRef.current.gain.value = voiceIsolationWeight;
            dryGainRef.current.gain.value = 1.0 - voiceIsolationWeight;
          } catch (e) {
            console.error("Failed to load RNNoise WASM:", e);
            source.connect(gateGainRef.current);
          }
        } else {
          source.connect(gateGainRef.current);
        }
      })
      .catch(console.error);

    return () => {
      isCancelled = true;
      if (streamRef.current) {
        streamRef.current.getTracks().forEach(t => t.stop());
        streamRef.current = null;
      }
      if (audioContextRef.current) {
        audioContextRef.current.close().catch(console.error);
        audioContextRef.current = null;
      }
    };
  }, [testing, voiceIsolation]);

  useEffect(() => {
    if (testing) {
      if (masterGainRef.current) masterGainRef.current.gain.value = volume;
      if (wetGainRef.current) wetGainRef.current.gain.value = voiceIsolationWeight;
      if (dryGainRef.current) dryGainRef.current.gain.value = 1.0 - voiceIsolationWeight;
    }
  }, [volume, voiceIsolationWeight, testing]);

  const thresholdPercent = noiseGateEnabled
    ? visualizerLevelFromRms(autoNoiseGate ? 0.01 : noiseGateThreshold) * 100
    : 0;

  return (
    <div className="mic-test">
      <div className="mic-visualizer" title="Mic gate preview">
        <div className="mic-threshold-line" style={{ bottom: `${thresholdPercent}%` }} />
        {meterSamples.map((sample, index) => (
          <span
            className={sample.open ? "mic-meter-bar open" : "mic-meter-bar cut"}
            key={index}
            style={{ height: `${Math.max(4, sample.level * 100)}%` }}
          />
        ))}
      </div>
      <button 
        className={testing ? "secondary-button active" : "secondary-button"} 
        onClick={() => setTesting(!testing)}
      >
        {testing ? "Stop Testing" : "Test Mic"}
      </button>
    </div>
  );
}

export function App() {
  const [activeTab, setActiveTab] = useState<Tab>("library");
  const [diagnostics, setDiagnostics] = useState(defaultDiagnostics);
  const [settings, setSettings] = useState<ClipSettings | undefined>();
  const [clips, setClips] = useState<ClipRecord[]>([]);
  const [query, setQuery] = useState("");
  const [notice, setNotice] = useState<AppNotice | undefined>();
  const [selectedClip, setSelectedClip] = useState<ClipRecord | undefined>();
  const [clipSounds, setClipSounds] = useState<ClipSoundOption[]>([]);
  const [updateState, setUpdateState] = useState<UpdateState>(defaultUpdateState);
  const [isSavingClip, setIsSavingClip] = useState(false);
  const [isExportingDiagnostics, setIsExportingDiagnostics] = useState(false);
  const [saveIoAnalyzer, setSaveIoAnalyzer] = useState<SaveIoAnalyzerState>({
    available: false,
    armed: false,
    traceReady: false
  });
  const clipSoundUrlsRef = useRef<Record<string, string>>({});

  function showNotice(message: string, tab?: Tab, durationMs = 4000) {
    setNotice({ message, tab, durationMs });
  }

  async function refresh() {
    const [nextDiagnostics, nextSettings, nextClips, nextClipSounds, nextSaveIoAnalyzer] = await Promise.all([
      window.clipture.getDiagnostics(),
      window.clipture.getSettings(),
      window.clipture.listClips(),
      window.clipture.listClipSounds(),
      window.clipture.getSaveIoAnalyzerState()
    ]);
    setDiagnostics(nextDiagnostics);
    setSettings(nextSettings);
    setClips(nextClips);
    setClipSounds(nextClipSounds);
    setSaveIoAnalyzer(nextSaveIoAnalyzer);
    clipSoundUrlsRef.current = Object.fromEntries(nextClipSounds.filter((sound) => sound.url).map((sound) => [sound.id, sound.url as string]));
  }

  useEffect(() => {
    void refresh().catch((error) => {
      console.error("Failed to refresh app state:", error);
      showNotice(error instanceof Error ? error.message : "Could not refresh app state.", undefined, 6000);
    });
    const timer = window.setInterval(() => {
      void window.clipture.getDiagnostics().then(setDiagnostics).catch((error) => {
        console.warn("Failed to refresh diagnostics:", error);
      });
    }, 2000);
    const unsubscribeLibrary = window.clipture.onLibraryChanged((addedClip) => {
      if (addedClip) {
        setClips((current) => [addedClip, ...current.filter((clip) => clip.id !== addedClip.id)]);
        return;
      }
      void window.clipture.listClips().then(setClips).catch((error) => {
        console.error("Failed to refresh library:", error);
      });
    });
    void window.clipture.getUpdateState().then(setUpdateState).catch((error) => {
      console.warn("Failed to read update state:", error);
    });
    const unsubscribeUpdates = window.clipture.onUpdateStateChanged(setUpdateState);
    const unsubscribeSound = window.clipture.onPlaySound((sound) => {
      const url = clipSoundUrlsRef.current[sound];
      if (url) {
        const audio = new Audio(url);
        audio.play().catch(console.error);
      }
    });
    return () => {
      window.clearInterval(timer);
      unsubscribeLibrary();
      unsubscribeUpdates();
      unsubscribeSound();
    };
  }, []);

  useEffect(() => {
    if (!notice) return;
    const timer = window.setTimeout(() => setNotice(undefined), notice.durationMs);
    return () => window.clearTimeout(timer);
  }, [notice]);

  useEffect(() => {
    if (!settings) return;
    cacheAndApplyUiTheme(settings);
  }, [settings?.uiTheme, settings?.customMainColor, settings?.customAccentColor]);

  async function saveClip() {
    if (isSavingClip) return;
    const length = settings?.clipLengthSeconds ?? 30;
    setIsSavingClip(true);
    try {
      const result = await window.clipture.saveClip(length);
      showNotice(
        result.saveIoAnalysis?.length ? result.message + " I/O trace ready." : result.message,
        activeTab
      );
      setSaveIoAnalyzer(await window.clipture.getSaveIoAnalyzerState());
      if (result.clip) {
        setClips((current) => [result.clip!, ...current.filter((clip) => clip.id !== result.clip!.id)]);
      }
    } catch (error) {
      showNotice(error instanceof Error ? error.message : "Could not save clip.", activeTab, 6000);
    } finally {
      setIsSavingClip(false);
      void window.clipture.getSaveIoAnalyzerState().then(setSaveIoAnalyzer).catch(() => {});
    }
  }

  async function toggleSaveIoAnalyzer() {
    if (!saveIoAnalyzer.available || isSavingClip) return;
    try {
      const nextState = await window.clipture.setSaveIoAnalyzerArmed(!saveIoAnalyzer.armed);
      setSaveIoAnalyzer(nextState);
      showNotice(nextState.armed ? "Next save I/O trace armed" : "I/O trace canceled", "diagnostics");
    } catch (error) {
      showNotice(error instanceof Error ? error.message : "Could not change I/O tracing.", "diagnostics", 6000);
    }
  }

  async function exportDiagnostics() {
    if (isExportingDiagnostics) return;
    setIsExportingDiagnostics(true);
    try {
      const filePath = await window.clipture.exportDiagnostics();
      if (filePath) showNotice(`Diagnostics exported to ${filePath}`, "diagnostics", 5000);
    } catch (error) {
      showNotice(error instanceof Error ? error.message : "Could not export diagnostics.", "diagnostics", 6000);
    } finally {
      setIsExportingDiagnostics(false);
    }
  }

  async function importVideos() {
    const imported = await window.clipture.importVideoFolders();
    if (imported) {
      showNotice("Imported video folder added", "library");
      await refresh();
    }
  }

  async function updateSettings(patch: Partial<ClipSettings>) {
    if (!settings) return;
    const nextSettings = { ...settings, ...patch };
    if (patch.uiTheme || patch.customMainColor || patch.customAccentColor) {
      cacheAndApplyUiTheme(nextSettings);
    }
    const saved = await window.clipture.saveSettings(nextSettings);
    setSettings(saved);
    showNotice("Settings saved", "settings", 2200);
  }

  function previewClipSound(sound: string) {
    const url = clipSoundUrlsRef.current[sound];
    if (!url || sound === "none") return;
    const audio = new Audio(url);
    audio.play().catch(console.error);
  }

  async function checkForUpdatesNow() {
    const nextState = await window.clipture.checkForUpdates();
    setUpdateState(nextState);
  }

  function installUpdate() {
    void window.clipture.installUpdate();
  }

  return (
    <div className="app-shell">
      <div className="titlebar-drag-region" aria-hidden="true" />
      <TitlebarUpdateControls 
            updateState={updateState} 
            onCheck={() => void checkForUpdatesNow()} 
            onDownload={() => void window.clipture.downloadUpdate()}
            onInstall={installUpdate} 
          />
      <aside className="sidebar">
        <div className="brand">
          <img src={logoUrl} alt="Clipture" className="mark" />
          <div>
            <strong>Clipture</strong>
          </div>
        </div>
        <button className={activeTab === "library" ? "nav active" : "nav"} onClick={() => setActiveTab("library")}>
          <Library size={18} /> Library
        </button>
        <button className={activeTab === "settings" ? "nav active" : "nav"} onClick={() => setActiveTab("settings")}>
          <SlidersHorizontal size={18} /> Settings
        </button>
        <button className={activeTab === "diagnostics" ? "nav active" : "nav"} onClick={() => setActiveTab("diagnostics")}>
          <Activity size={18} /> Diagnostics
        </button>
        <div className={diagnostics.degraded ? "encoder degraded" : "encoder"}>
          <span>Encoder</span>
          <strong>{diagnostics.activeEncoder}</strong>
          <small>{diagnostics.encoderMode}</small>
          <small>{diagnostics.gpu}</small>
        </div>
      </aside>

      <main className="workspace">
        {activeTab !== "library" && (
          <header className="topbar">
            <div>
              <h1>{activeTab === "settings" ? "Settings" : "Diagnostics"}</h1>
              {activeTab === "diagnostics" && <p>{diagnostics.status}</p>}
            </div>
            <div className="topbar-actions">
              <button className="primary" onClick={saveClip} disabled={isSavingClip}>
                <Save size={18} /> {isSavingClip ? "Saving..." : `Save last ${settings?.clipLengthSeconds ?? 30}s`}
              </button>
              {activeTab === "diagnostics" && (
                <button
                  className="secondary-button"
                  onClick={() => void exportDiagnostics()}
                  disabled={isExportingDiagnostics}
                >
                  <Download size={18} /> {isExportingDiagnostics ? "Exporting..." : "Export diagnostics"}
                </button>
              )}
              {activeTab === "diagnostics" && saveIoAnalyzer.available && (
                <button
                  className="secondary-button"
                  onClick={() => void toggleSaveIoAnalyzer()}
                  disabled={isSavingClip}
                >
                  <Activity size={18} /> {saveIoAnalyzer.armed ? "Cancel I/O trace" : "Analyze next save"}
                </button>
              )}
            </div>
          </header>
        )}

        {notice && (!notice.tab || notice.tab === activeTab) && (
          <div className="notice" role="status">{notice.message}</div>
        )}
        {activeTab === "library" && (
          <LibraryView
            clips={clips}
            query={query}
            setQuery={setQuery}
            selectedClip={selectedClip}
            setSelectedClip={setSelectedClip}
            settings={settings}
            onSaveClip={saveClip}
            onImportVideos={importVideos}
            isSavingClip={isSavingClip}
            clipLengthSeconds={settings?.clipLengthSeconds ?? 30}
          />
        )}
        {activeTab === "settings" && settings && (
          <SettingsView
            settings={settings}
            clipSounds={clipSounds}
            onChange={updateSettings}
            onPreviewSound={previewClipSound}
            onImportSound={async () => {
              const sound = await window.clipture.importClipSound();
              if (!sound) return;
              await refresh();
              await updateSettings({ clipSound: sound.id });
              previewClipSound(sound.id);
            }}
            onRevealSounds={() => window.clipture.revealSoundsFolder()}
          />
        )}
        {activeTab === "diagnostics" && <DiagnosticsView diagnostics={diagnostics} />}
      </main>
    </div>
  );
}

function LibraryView({
  clips,
  query,
  setQuery,
  selectedClip,
  setSelectedClip,
  settings,
  onSaveClip,
  onImportVideos,
  isSavingClip,
  clipLengthSeconds
}: {
  clips: ClipRecord[];
  query: string;
  setQuery: (value: string) => void;
  selectedClip: ClipRecord | undefined;
  setSelectedClip: (clip: ClipRecord | undefined) => void;
  settings?: ClipSettings;
  onSaveClip: () => void;
  onImportVideos: () => Promise<void>;
  isSavingClip: boolean;
  clipLengthSeconds: number;
}) {
  const [libraryTab, setLibraryTab] = useState<"clips" | "imported">("clips");
  const [folderFilter, setFolderFilter] = useState("");
  const [selectionMode, setSelectionMode] = useState(false);
  const [selectedClipIds, setSelectedClipIds] = useState<Set<string>>(new Set());
  const [editorialPreviewId, setEditorialPreviewId] = useState("");

  const savedClips = useMemo(() => clips.filter((clip) => clip.librarySource !== "imported"), [clips]);
  const importedClips = useMemo(() => clips.filter((clip) => clip.librarySource === "imported"), [clips]);
  const tabClips = libraryTab === "clips" ? savedClips : importedClips;
  const folderFilters = useMemo(() => {
    const seen = new Set<string>();
    return tabClips
      .map((clip) => clip.folderName || clip.gameOrApp || "Clips")
      .filter((folder) => {
        const key = folder.toLowerCase();
        if (!folder || seen.has(key)) return false;
        seen.add(key);
        return true;
      })
      .sort((a, b) => a.localeCompare(b));
  }, [tabClips]);

  useEffect(() => {
    if (folderFilter && !folderFilters.includes(folderFilter)) setFolderFilter("");
  }, [folderFilter, folderFilters]);

  useEffect(() => {
    cancelSelection();
    if (selectedClip) {
      const selectedIsImported = selectedClip.librarySource === "imported";
      if ((libraryTab === "imported") !== selectedIsImported) setSelectedClip(undefined);
    }
  }, [libraryTab]);

  const filteredClips = useMemo(() => {
    const trimmedQuery = query.trim().toLowerCase();
    return tabClips.filter((clip) => {
      const sourceLabels = clipSourceLabels(clip, settings);
      const folder = clip.folderName || clip.gameOrApp || "";
      const matchesFolder = !folderFilter || folder === folderFilter;
      const haystack = [
        clip.title,
        clip.gameOrApp,
        folder,
        clip.encoder,
        ...sourceLabels,
        ...clip.audioTracks
      ].join(" ").toLowerCase();
      return matchesFolder && (!trimmedQuery || haystack.includes(trimmedQuery));
    });
  }, [folderFilter, query, settings, tabClips]);

  const isEditorialLibrary = settings?.uiTheme === "glitten";
  const editorialPreviewClip = filteredClips.find((clip) => clip.id === editorialPreviewId)
    ?? filteredClips[0];

  const playEditorialClip = (clip: ClipRecord) => {
    setEditorialPreviewId(clip.id);
    setSelectedClip(clip);
  };

  const previewEditorialClip = (clip: ClipRecord) => {
    setEditorialPreviewId(clip.id);
    setSelectedClip(undefined);
  };

  const selectedCount = selectedClipIds.size;
  const allFilteredClipsSelected = filteredClips.length > 0
    && filteredClips.every((clip) => selectedClipIds.has(clip.id));

  const toggleClipSelection = (clipId: string) => {
    setSelectedClipIds((current) => {
      const next = new Set(current);
      if (next.has(clipId)) next.delete(clipId);
      else next.add(clipId);
      return next;
    });
  };

  const cancelSelection = () => {
    setSelectionMode(false);
    setSelectedClipIds(new Set());
  };

  const toggleSelectAllFilteredClips = () => {
    setSelectedClipIds((current) => {
      const next = new Set(current);
      if (filteredClips.every((clip) => next.has(clip.id))) {
        filteredClips.forEach((clip) => next.delete(clip.id));
      } else {
        filteredClips.forEach((clip) => next.add(clip.id));
      }
      return next;
    });
  };

  const deleteSelectedClips = async () => {
    const ids = [...selectedClipIds];
    if (ids.length === 0) return;
    const deleted = await window.clipture.deleteClips(ids);
    if (deleted) {
      if (selectedClip && selectedClipIds.has(selectedClip.id)) setSelectedClip(undefined);
      cancelSelection();
    }
  };

  const handleImportVideos = async () => {
    await onImportVideos();
    setLibraryTab("imported");
  };

  const emptyTitle = libraryTab === "clips" ? "No clips yet" : "No imported videos yet";
  const emptyCopy = libraryTab === "clips"
    ? "Your saved clips will appear here."
    : "Choose a folder and Clipture will read videos from it without copying them.";
  const emptyDetail = libraryTab === "clips"
    ? "Use the in-game overlay to create clips while you play."
    : "Imported videos stay in their original folders.";

  return (
    <div className="library-layout">
      <section className="browse-surface">
        <div className="library-top">
          <div className="library-hero">
            <div className="library-title">
              <Clapperboard className="library-title-icon" size={28} />
              <h1>
                <span className="library-heading-default">Clip Library</span>
                <span className="library-heading-glitten">Clips</span>
              </h1>
            </div>
            <button className="primary library-save-button" onClick={onSaveClip} disabled={isSavingClip}>
              <Save size={18} /> {isSavingClip ? "Saving..." : `Save last ${clipLengthSeconds}s`}
            </button>
          </div>

          <div className="library-tabs" role="tablist" aria-label="Library sections">
            <button
              className={libraryTab === "clips" ? "library-tab active" : "library-tab"}
              type="button"
              onClick={() => setLibraryTab("clips")}
            >
              Clips
            </button>
            <button
              className={libraryTab === "imported" ? "library-tab active" : "library-tab"}
              type="button"
              onClick={() => setLibraryTab("imported")}
            >
              Imported Videos
            </button>
          </div>

          <label className="library-search">
            <Search size={22} />
            <input value={query} onChange={(event) => setQuery(event.target.value)} placeholder="Filter by game, app, track, or title" />
          </label>

          <div className="library-actions-row">
            <div className="chip-row">
              {folderFilters.map((folder) => (
                <button
                  className={folderFilter === folder ? "chip active" : "chip"}
                  key={folder}
                  onClick={() => setFolderFilter(folderFilter === folder ? "" : folder)}
                  type="button"
                >
                  {folder}
                </button>
              ))}
            </div>
            {selectionMode ? (
              <div className="selection-bar">
                <strong>{selectedCount} selected</strong>
                <span className="selection-divider" />
                <button className="secondary-button select-all-button" type="button" onClick={toggleSelectAllFilteredClips} disabled={filteredClips.length === 0}>
                  <span className={allFilteredClipsSelected ? "select-clips-empty-box checked" : "select-clips-empty-box"} aria-hidden="true">
                    {allFilteredClipsSelected && <Check size={13} />}
                  </span>
                  {allFilteredClipsSelected ? "Deselect all" : "Select all"}
                </button>
                <button className="secondary-button" type="button" onClick={cancelSelection}>Cancel</button>
                <button className="danger-button" type="button" onClick={() => void deleteSelectedClips()} disabled={selectedCount === 0}>
                  <Trash2 size={18} /> Delete
                </button>
              </div>
            ) : (
              <div className="library-inline-actions">
                <button className="secondary-button import-videos-button" type="button" onClick={() => void handleImportVideos()}>
                  <Upload size={18} /> Import videos
                </button>
                <button
                  className="secondary-button select-clips-button"
                  type="button"
                  onClick={() => {
                    setSelectedClip(undefined);
                    setSelectionMode(true);
                  }}
                >
                  <span className="select-clips-empty-box" aria-hidden="true" />
                  Select clips
                </button>
              </div>
            )}
          </div>
        </div>

        {filteredClips.length === 0 ? (
          <LibraryEmptyState
            title={emptyTitle}
            copy={emptyCopy}
            detail={emptyDetail}
            actionLabel={libraryTab === "clips" ? "Save your first clip" : "Import videos"}
            onAction={libraryTab === "clips" ? onSaveClip : handleImportVideos}
          />
        ) : isEditorialLibrary && !selectionMode && editorialPreviewClip ? (
          selectedClip ? (
            <ClipPlayer
              clip={selectedClip}
              onClose={() => {
                setEditorialPreviewId(selectedClip.id);
                setSelectedClip(undefined);
              }}
              onSelectClip={playEditorialClip}
              railClips={filteredClips}
              settings={settings}
            />
          ) : (
            <LibraryClipPreview
              clip={editorialPreviewClip}
              railClips={filteredClips}
              settings={settings}
              onPlay={() => playEditorialClip(editorialPreviewClip)}
              onSelectClip={previewEditorialClip}
            />
          )
        ) : selectedClip ? (
          <ClipPlayer
            clip={selectedClip}
            onClose={() => setSelectedClip(undefined)}
            settings={settings}
          />
        ) : (
          <div className="clip-grid">
            {filteredClips.map((clip) => (
              <ClipCard
                clip={clip}
                isActive={false}
                isSelected={selectedClipIds.has(clip.id)}
                key={clip.id}
                onPlay={() => {
                  setEditorialPreviewId(clip.id);
                  setSelectedClip(clip);
                }}
                onToggleSelected={() => toggleClipSelection(clip.id)}
                selectionMode={selectionMode}
                settings={settings}
              />
            ))}
          </div>
        )}
      </section>
    </div>
  );
}

function LibraryEmptyState({
  title,
  copy,
  detail,
  actionLabel,
  onAction
}: {
  title: string;
  copy: string;
  detail: string;
  actionLabel: string;
  onAction: () => void | Promise<void>;
}) {
  return (
    <div className="library-empty-state">
      <div className="library-empty-art" aria-hidden="true">
        <Clapperboard size={74} />
      </div>
      <h2>{title}</h2>
      <p>{copy}</p>
      <p>{detail}</p>
      <button className="secondary-button library-empty-action" type="button" onClick={() => void onAction()}>
        <Clapperboard size={20} /> {actionLabel}
      </button>
    </div>
  );
}

function uniqueLabels(labels: Array<string | null | undefined>): string[] {
  const seen = new Set<string>();
  return labels
    .map((label) => (label ?? "").trim())
    .filter((label) => {
      const key = label.toLowerCase();
      if (!label || seen.has(key)) return false;
      seen.add(key);
      return true;
    });
}

function clipSourceLabels(clip: ClipRecord, settings?: ClipSettings): string[] {
  if (clip.focusedApps && clip.focusedApps.length > 0) {
    return uniqueLabels(clip.focusedApps);
  }

  const bgApps = new Set(
    settings?.audioSources
      .filter((source) => source.kind === "app" && source.processName)
      .flatMap((source) => [`app:${source.processName}`, source.processName!.replace(/\.exe$/i, "")]) || []
  );
  const activeTracks = clip.audioTracks
    .filter((track) =>
      track !== "system-loopback-pcm" &&
      track !== "System audio" &&
      track !== "microphone-pcm" &&
      track !== "Microphone" &&
      track !== "mixed-preview-pcm" &&
      !bgApps.has(track)
    )
    .map((track) =>
      track.startsWith("app:")
        ? track.substring(4).replace(/\.exe$/i, "")
        : track.startsWith("game:")
          ? track.substring(5).replace(/\.exe$/i, "")
          : track
    );

  return uniqueLabels([
    clip.gameOrApp !== "Foreground app" ? clip.gameOrApp : null,
    ...activeTracks
  ]);
}

function useNearViewport<T extends Element>(delayMs = 75): [React.MutableRefObject<T | null>, boolean] {
  const elementRef = useRef<T | null>(null);
  const [intersecting, setIntersecting] = useState(false);
  const [ready, setReady] = useState(false);

  useEffect(() => {
    const element = elementRef.current;
    if (!element || typeof IntersectionObserver === "undefined") {
      setIntersecting(true);
      return;
    }
    const observer = new IntersectionObserver(
      (entries) => setIntersecting(entries.some((entry) => entry.isIntersecting)),
      { rootMargin: "700px 0px" }
    );
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    if (!intersecting) {
      setReady(false);
      return;
    }
    const timer = window.setTimeout(() => setReady(true), delayMs);
    return () => window.clearTimeout(timer);
  }, [delayMs, intersecting]);

  return [elementRef, ready];
}

function useClipIconUrl(clip: ClipRecord, preferredLabels: string[], enabled = true): string {
  const [iconUrl, setIconUrl] = useState("");
  const focusedAppsKey = (clip.focusedApps ?? []).join("|");
  const audioTracksKey = clip.audioTracks.join("|");
  const preferredLabelsKey = preferredLabels.join("|");

  useEffect(() => {
    let active = true;
    if (!enabled) {
      setIconUrl("");
      return () => {
        active = false;
      };
    }
    if (preferredLabels.some((label) => label.trim().toLowerCase() === "clipture")) {
      setIconUrl(logoUrl);
      return () => {
        active = false;
      };
    }
    setIconUrl("");
    window.clipture.clipIconUrl(clip, preferredLabels).then((url) => {
      if (active) setIconUrl(url || "");
    }).catch(() => {
      if (active) setIconUrl("");
    });
    return () => {
      active = false;
    };
  }, [clip.id, clip.gameOrApp, focusedAppsKey, audioTracksKey, preferredLabelsKey, enabled]);

  return iconUrl;
}

function ClipCard({
  clip,
  isActive,
  isSelected,
  onPlay,
  onToggleSelected,
  selectionMode,
  settings
}: {
  clip: ClipRecord;
  isActive: boolean;
  isSelected: boolean;
  onPlay: () => void;
  onToggleSelected: () => void;
  selectionMode: boolean;
  settings?: ClipSettings;
}) {
  const createdAt = parseClipDate(clip.createdAt);
  const displayTitle = clip.title === "Clipture clip" ? "Clipture" : clip.title;
  const [thumbnailUrl, setThumbnailUrl] = useState<string>("");
  const [cardRef, loadMedia] = useNearViewport<HTMLElement>();
  const [isEditingTitle, setIsEditingTitle] = useState(false);
  const [editTitle, setEditTitle] = useState(displayTitle);
  const sourceLabels = useMemo(() => clipSourceLabels(clip, settings), [clip, settings]);
  const iconUrl = useClipIconUrl(clip, sourceLabels, loadMedia);

  useEffect(() => {
    let active = true;
    if (!loadMedia) {
      setThumbnailUrl("");
      return () => {
        active = false;
      };
    }
    window.clipture.clipThumbnailUrl(clip.filePath).then((url) => {
      if (active && url) setThumbnailUrl(url);
    });
    return () => {
      active = false;
    };
  }, [clip.filePath, loadMedia]);

  const handleRename = async () => {
    const newTitle = editTitle.trim() || "Clipture";
    if (newTitle !== clip.title) {
      const success = await window.clipture.renameClip(clip.id, newTitle);
      if (success) {
        clip.title = newTitle; // optimistically update
      }
    }
    setEditTitle(newTitle);
    setIsEditingTitle(false);
  };

  useEffect(() => {
    if (isEditingTitle && editTitle !== clip.title) {
      const timer = setTimeout(() => {
        void handleRename();
      }, 3000);
      return () => clearTimeout(timer);
    }
  }, [editTitle, isEditingTitle, clip.title]);

  return (
    <article ref={cardRef} className={[isActive ? "clip-card active" : "clip-card", isSelected ? "selected" : "", selectionMode ? "selectable" : ""].filter(Boolean).join(" ")}>
      <button className="thumbnail-button" onClick={selectionMode ? onToggleSelected : onPlay}>
        {selectionMode && (
          <span className={isSelected ? "clip-select-box checked" : "clip-select-box"}>
            {isSelected && <Check size={18} />}
          </span>
        )}
        {thumbnailUrl ? <img src={thumbnailUrl} alt="" loading="eager" decoding="async" /> : <div className="thumbnail-skeleton" aria-hidden="true" />}
        {clip.durationSeconds > 0 && <span className="duration-badge">{formatDuration(clip.durationSeconds)}</span>}
        {!selectionMode && (
          <span className="play-badge">
            <Play size={18} />
          </span>
        )}
      </button>
      <div className="clip-info">
        <div className="clip-title-container">
          {isEditingTitle ? (
            <input
              type="text"
              className="clip-name-input"
              value={editTitle}
              autoFocus
              onChange={(e) => setEditTitle(e.target.value)}
              onFocus={(e) => e.target.select()}
              onBlur={handleRename}
              onKeyDown={(e) => {
                if (e.key === "Enter") handleRename();
                if (e.key === "Escape") {
                  setEditTitle(clip.title);
                  setIsEditingTitle(false);
                }
              }}
            />
          ) : (
            <div className="clip-title-display" onDoubleClick={() => setIsEditingTitle(true)}>
              {iconUrl && <img className="clip-app-icon" src={iconUrl} alt="" />}
              <span className="clip-name" title="Double click to rename">{displayTitle}</span>
              <button className="icon-button edit-title-button" title="Rename clip" onClick={(e) => { e.stopPropagation(); setIsEditingTitle(true); }}>
                <Edit3 size={15} />
              </button>
            </div>
          )}
        </div>
        <span className="clip-timestamp">{formatClipDate(createdAt)} <span>{formatClipTime(createdAt)}</span></span>
        <div className="clip-card-footer">
          <span><Clock size={14} /> {clip.fps > 0 ? `${clip.fps} FPS` : "Imported"}</span>
          <span>{clip.audioTracks.length} audio</span>
          <button className="icon-button" title="Reveal clip" onClick={(event) => { event.stopPropagation(); window.clipture.revealClip(clip.filePath); }}>
            <FolderOpen size={16} />
          </button>
        </div>
      </div>
    </article>
  );
}

function ClipRailItem({
  clip,
  active,
  onSelect
}: {
  clip: ClipRecord;
  active: boolean;
  onSelect: () => void;
}) {
  const [thumbnailUrl, setThumbnailUrl] = useState("");
  const [itemRef, loadMedia] = useNearViewport<HTMLButtonElement>(20);
  const createdAt = parseClipDate(clip.createdAt);
  const displayTitle = clip.title === "Clipture clip" ? "Clipture" : clip.title;

  useEffect(() => {
    let mounted = true;
    if (!loadMedia) {
      setThumbnailUrl("");
      return () => {
        mounted = false;
      };
    }
    void window.clipture.clipThumbnailUrl(clip.filePath).then((url) => {
      if (mounted) setThumbnailUrl(url || "");
    });
    return () => {
      mounted = false;
    };
  }, [clip.filePath, loadMedia]);

  useEffect(() => {
    if (active) itemRef.current?.scrollIntoView({ block: "nearest", behavior: "smooth" });
  }, [active]);

  return (
    <button
      ref={itemRef}
      className={active ? "clip-rail-item active" : "clip-rail-item"}
      type="button"
      aria-current={active ? "true" : undefined}
      onClick={onSelect}
    >
      <span className="clip-rail-thumbnail">
        {thumbnailUrl
          ? <img src={thumbnailUrl} alt="" loading="lazy" decoding="async" />
          : <span className="thumbnail-skeleton" aria-hidden="true" />}
        {clip.durationSeconds > 0 && (
          <span className="clip-rail-duration">{formatDuration(clip.durationSeconds)}</span>
        )}
      </span>
      <span className="clip-rail-copy">
        <strong>{displayTitle}</strong>
        <span>{formatClipDate(createdAt)} | {formatClipTime(createdAt)}</span>
      </span>
    </button>
  );
}

function LibraryPlayerSidebar({
  clip,
  railClips,
  settings,
  onSelectClip,
  onClose
}: {
  clip: ClipRecord;
  railClips: ClipRecord[];
  settings?: ClipSettings;
  onSelectClip: (clip: ClipRecord) => void;
  onClose?: () => void;
}) {
  const createdAt = parseClipDate(clip.createdAt);
  const displayTitle = clip.title === "Clipture clip" ? "Clipture" : clip.title;
  const sourceLabels = useMemo(() => clipSourceLabels(clip, settings), [clip, settings]);
  const sourceText = sourceLabels.length > 0 ? sourceLabels.join(", ") : clip.gameOrApp;
  const iconUrl = useClipIconUrl(clip, sourceLabels);

  return (
    <aside className="library-player-sidebar">
      <div className="library-player-side-header">
        <div className="library-player-side-title-row">
          <h2 className="library-player-side-title">
            {iconUrl && <img className="clip-app-icon" src={iconUrl} alt="" />}
            <span>{displayTitle}</span>
          </h2>
          {onClose && (
            <button className="icon-button" title="Close player" onClick={onClose}>
              <X size={17} />
            </button>
          )}
        </div>
        <p>{sourceText}</p>
        <div className="library-player-side-meta">
          <span>{formatClipDate(createdAt)} | {formatClipTime(createdAt)}</span>
          <span>{formatDuration(clip.durationSeconds)} | {clip.resolution} | {clip.fps} FPS</span>
          <span>{clip.audioTracks.length} audio</span>
        </div>
      </div>
      <div className="clip-rail-heading">
        <strong>More clips</strong>
        <span>{railClips.length}</span>
      </div>
      <div className="clip-rail" aria-label="More clips">
        {railClips.map((candidate) => (
          <ClipRailItem
            active={candidate.id === clip.id}
            clip={candidate}
            key={candidate.id}
            onSelect={() => onSelectClip(candidate)}
          />
        ))}
      </div>
    </aside>
  );
}

function LibraryClipPreview({
  clip,
  railClips,
  settings,
  onPlay,
  onSelectClip
}: {
  clip: ClipRecord;
  railClips: ClipRecord[];
  settings?: ClipSettings;
  onPlay: () => void;
  onSelectClip: (clip: ClipRecord) => void;
}) {
  const [thumbnailUrl, setThumbnailUrl] = useState("");

  useEffect(() => {
    let mounted = true;
    setThumbnailUrl("");
    void window.clipture.clipThumbnailUrl(clip.filePath).then((url) => {
      if (mounted) setThumbnailUrl(url || "");
    });
    return () => {
      mounted = false;
    };
  }, [clip.filePath]);

  return (
    <section className="player panel library-player library-player-preview">
      <button className="library-player-main library-preview-main" type="button" onClick={onPlay} aria-label={`Play ${clip.title}`}>
        {thumbnailUrl
          ? <img src={thumbnailUrl} alt="" decoding="async" />
          : <span className="thumbnail-skeleton" aria-hidden="true" />}
        <span className="play-badge library-preview-play"><Play size={20} /></span>
        {clip.durationSeconds > 0 && (
          <span className="duration-badge">{formatDuration(clip.durationSeconds)}</span>
        )}
      </button>
      <LibraryPlayerSidebar
        clip={clip}
        railClips={railClips}
        settings={settings}
        onSelectClip={onSelectClip}
      />
    </section>
  );
}

function displayAudioTrackName(track: string) {
  if (track === "microphone-pcm") return "Microphone";
  if (track === "system-loopback-pcm") return "System audio";
  if (track === "mixed-preview-pcm") return "Mixed preview";
  if (track.startsWith("app:")) return track.slice(4).replace(/\.exe$/i, "");
  if (track.startsWith("game:")) return track.slice(5).replace(/\.exe$/i, "");
  return track;
}

function displayAudioTracks(tracks: string[]) {
  return tracks.map(displayAudioTrackName).join(", ");
}

function parseClipDate(value: string) {
  const numeric = /^\d+$/.test(value) ? Number(value) : Number.NaN;
  const date = Number.isFinite(numeric)
    ? new Date(value.length <= 10 ? numeric * 1000 : numeric)
    : new Date(value);
  return Number.isNaN(date.getTime()) ? undefined : date;
}

function formatClipDate(date: Date | undefined) {
  if (!date) return "Unknown date";
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, "0");
  const day = String(date.getDate()).padStart(2, "0");
  return `${year}/${month}/${day}`;
}

function formatClipTime(date: Date | undefined) {
  if (!date) return "Unknown time";
  return new Intl.DateTimeFormat("en-US", {
    hour: "numeric",
    minute: "2-digit",
    second: "2-digit",
    hour12: true
  }).format(date);
}

function formatDuration(seconds: number) {
  const safeSeconds = Math.max(0, Math.round(seconds));
  const days = Math.floor(safeSeconds / 86400);
  const hours = Math.floor((safeSeconds % 86400) / 3600);
  const minutes = Math.floor((safeSeconds % 3600) / 60);
  const remainder = safeSeconds % 60;
  const clock = `${String(minutes).padStart(2, "0")}:${String(remainder).padStart(2, "0")}`;
  if (days > 0) return `${days}d ${String(hours).padStart(2, "0")}:${clock}`;
  if (hours > 0) return `${hours}:${clock}`;
  return `${minutes}:${String(remainder).padStart(2, "0")}`;
}

type MixedAudioChunk = {
  start: number;
  buffer: AudioBuffer;
  lastUsedAt: number;
};

type MixedAudioChunkRequest = {
  promise: Promise<void>;
  controller: AbortController;
};

const mixedAudioScheduleLookaheadSeconds = 0.5;
const mixedAudioSchedulerIntervalMs = 100;

function acceleratorFromKeyboardEvent(event: KeyboardEvent) {
  const ignoredKeys = new Set(["Control", "Shift", "Alt", "Meta", "OS"]);
  if (ignoredKeys.has(event.key)) return "";

  const parts: string[] = [];
  if (event.ctrlKey) parts.push("Ctrl");
  if (event.altKey) parts.push("Alt");
  if (event.shiftKey) parts.push("Shift");
  if (event.metaKey) parts.push("Super");

  const specialKeys: Record<string, string> = {
    " ": "Space",
    ArrowUp: "Up",
    ArrowDown: "Down",
    ArrowLeft: "Left",
    ArrowRight: "Right",
    Escape: "Esc",
    "+": "Plus"
  };
  const key = specialKeys[event.key] ?? (event.key.length === 1 ? event.key.toUpperCase() : event.key);
  if (!key || key === "Esc") return "";
  parts.push(key);
  return parts.join("+");
}

function ClipPlayer({
  clip,
  onClose,
  settings,
  railClips,
  onSelectClip
}: {
  clip: ClipRecord;
  onClose: () => void;
  settings?: ClipSettings;
  railClips?: ClipRecord[];
  onSelectClip?: (clip: ClipRecord) => void;
}) {
  const videoRef = useRef<HTMLVideoElement | null>(null);
  const fastHoldActivatedRef = useRef(false);
  const holdTimeoutRef = useRef<number | null>(null);
  const videoClickTimeoutRef = useRef<number | null>(null);
  const keyboardSeekDelayRef = useRef<number | null>(null);
  const keyboardSeekIntervalRef = useRef<number | null>(null);
  const keyboardSeekDirectionRef = useRef<-1 | 0 | 1>(0);
  const keyboardSeekStartedAtRef = useRef(0);
  const keyboardSeekingRef = useRef(false);
  const seekFeedbackTotalRef = useRef(0);
  const seekFeedbackSequenceRef = useRef(0);
  const seekFeedbackTimeoutRef = useRef<number | null>(null);
  const mixedPlayRequestRef = useRef(0);
  const playbackRequestedRef = useRef(true);
  const [src, setSrc] = useState("");
  const [message, setMessage] = useState("Preparing playback");
  const [mixedPlayback, setMixedPlayback] = useState(false);
  const [mixedAudioChunkUrl, setMixedAudioChunkUrl] = useState("");
  const [mixedAudioChunkSeconds, setMixedAudioChunkSeconds] = useState(8);
  const [playing, setPlaying] = useState(false);
  const [holdingFast, setHoldingFast] = useState(false);
  const [controlsVisible, setControlsVisible] = useState(true);
  const controlsTimeoutRef = useRef<number | null>(null);
  const [currentTime, setCurrentTime] = useState(0);
  const [duration, setDuration] = useState(clip.durationSeconds);
  const [volume, setVolume] = useState(1);
  const [seekFeedback, setSeekFeedback] = useState<{ direction: -1 | 1; seconds: number; sequence: number } | null>(null);
  const sourceLabels = useMemo(() => clipSourceLabels(clip, settings), [clip, settings]);
  const sourceText = sourceLabels.length > 0 ? sourceLabels.join(", ") : clip.gameOrApp;
  const iconUrl = useClipIconUrl(clip, sourceLabels);
  const embeddedInLibrary = Boolean(railClips && onSelectClip);
  const displayTitle = clip.title === "Clipture clip" ? "Clipture" : clip.title;

  useEffect(() => {
    let active = true;
    setSrc("");
    setMessage("Preparing playback");
    setMixedPlayback(false);
    setMixedAudioChunkUrl("");
    setMixedAudioChunkSeconds(8);
    setPlaying(false);
    setHoldingFast(false);
    setCurrentTime(0);
    setDuration(clip.durationSeconds);
    setSeekFeedback(null);
    playbackRequestedRef.current = true;
    mixedPlayRequestRef.current += 1;
    seekFeedbackTotalRef.current = 0;
    void window.clipture.clipPlaybackUrl(clip.filePath, clip.audioTracks).then((result) => {
      if (!active) return;
      setSrc(result.url);
      setMessage(result.message);
      setMixedPlayback(result.mixed && Boolean(result.audioChunkUrl));
      setMixedAudioChunkUrl(result.audioChunkUrl || "");
      setMixedAudioChunkSeconds(result.audioChunkSeconds || 8);
    });
    return () => {
      active = false;
      void window.clipture.releasePlaybackCache();
    };
  }, [clip.audioTracks, clip.filePath]);

  const audioCtxRef = useRef<AudioContext | null>(null);
  const gainNodeRef = useRef<GainNode | null>(null);
  const connectedVideoRef = useRef<HTMLVideoElement | null>(null);
  const mixedChunkCacheRef = useRef<Map<number, MixedAudioChunk>>(new Map());
  const mixedChunkRequestsRef = useRef<Map<number, MixedAudioChunkRequest>>(new Map());
  const mixedScheduledChunksRef = useRef<Map<number, AudioBufferSourceNode[]>>(new Map());
  const mixedTimerRef = useRef<number | null>(null);
  const mixedGenerationRef = useRef(0);
  const mixedPrimingPlayRef = useRef(false);

  const ensureAudioContext = () => {
    if (!audioCtxRef.current || audioCtxRef.current.state === "closed") {
      const AudioContextCtor = window.AudioContext || (window as any).webkitAudioContext;
      audioCtxRef.current = new AudioContextCtor();
      gainNodeRef.current = null;
      connectedVideoRef.current = null;
    }
    if (!gainNodeRef.current || gainNodeRef.current.context !== audioCtxRef.current) {
      gainNodeRef.current = audioCtxRef.current.createGain();
      gainNodeRef.current.connect(audioCtxRef.current.destination);
    }
    return audioCtxRef.current;
  };

  const stopMixedNodes = (nodes: AudioBufferSourceNode[]) => {
    for (const node of nodes) {
      try {
        node.stop();
      } catch {
        // Already stopped.
      }
      try {
        node.disconnect();
      } catch {
        // Already disconnected.
      }
    }
  };

  const stopScheduledMixedChunk = (chunkIndex: number) => {
    const nodes = mixedScheduledChunksRef.current.get(chunkIndex);
    if (!nodes) return;
    stopMixedNodes(nodes);
    mixedScheduledChunksRef.current.delete(chunkIndex);
  };

  const stopMixedAudio = () => {
    for (const nodes of mixedScheduledChunksRef.current.values()) {
      stopMixedNodes(nodes);
    }
    mixedScheduledChunksRef.current.clear();
  };

  const resetMixedAudioTimeline = () => {
    mixedGenerationRef.current += 1;
    stopMixedAudio();
    for (const request of mixedChunkRequestsRef.current.values()) {
      request.controller.abort();
    }
    mixedChunkRequestsRef.current.clear();
  };

  const clearMixedAudio = () => {
    resetMixedAudioTimeline();
    mixedChunkCacheRef.current.clear();
  };

  const mixedChunkUrl = (chunkIndex: number) => {
    const chunkSeconds = Math.max(1, mixedAudioChunkSeconds || 8);
    const start = Math.max(0, chunkIndex * chunkSeconds);
    const remaining = duration > 0 ? Math.max(0.25, duration - start) : chunkSeconds;
    const chunkDuration = Math.min(chunkSeconds, remaining);
    const separator = mixedAudioChunkUrl.includes("?") ? "&" : "?";
    return `${mixedAudioChunkUrl}${separator}start=${encodeURIComponent(start.toFixed(3))}&duration=${encodeURIComponent(chunkDuration.toFixed(3))}`;
  };

  const mixedChunkIndexForTime = (time: number) => {
    const chunkSeconds = Math.max(1, mixedAudioChunkSeconds || 8);
    return Math.max(0, Math.floor(Math.max(0, time) / chunkSeconds));
  };

  const loadMixedChunk = (chunkIndex: number, generation: number) => {
    if (!mixedPlayback || !mixedAudioChunkUrl) return Promise.resolve();
    if (mixedChunkCacheRef.current.has(chunkIndex)) return Promise.resolve();
    const pending = mixedChunkRequestsRef.current.get(chunkIndex);
    if (pending) return pending.promise;

    const chunkSeconds = Math.max(1, mixedAudioChunkSeconds || 8);
    const chunkStart = chunkIndex * chunkSeconds;
    if (duration > 0 && chunkStart > duration + 0.25) return Promise.resolve();

    const controller = new AbortController();
    let request: Promise<void>;
    request = fetch(mixedChunkUrl(chunkIndex), { signal: controller.signal })
      .then(async (response) => {
        if (!response.ok) throw new Error(`audio chunk ${response.status}`);
        const arrayBuffer = await response.arrayBuffer();
        if (generation !== mixedGenerationRef.current || arrayBuffer.byteLength === 0) return;
        const ctx = ensureAudioContext();
        const buffer = await ctx.decodeAudioData(arrayBuffer.slice(0));
        if (generation !== mixedGenerationRef.current) return;
        mixedChunkCacheRef.current.set(chunkIndex, {
          start: chunkStart,
          buffer,
          lastUsedAt: performance.now()
        });
      })
      .catch((error) => {
        if (error instanceof DOMException && error.name === "AbortError") return;
        console.warn("Mixed audio chunk failed:", error);
      })
      .finally(() => {
        if (mixedChunkRequestsRef.current.get(chunkIndex)?.promise === request) {
          mixedChunkRequestsRef.current.delete(chunkIndex);
        }
      });

    mixedChunkRequestsRef.current.set(chunkIndex, { promise: request, controller });
    return request;
  };

  const cleanupMixedChunks = (time: number) => {
    const chunkSeconds = Math.max(1, mixedAudioChunkSeconds || 8);
    for (const [chunkIndex, chunk] of mixedChunkCacheRef.current.entries()) {
      const chunkEnd = chunk.start + chunk.buffer.duration;
      if (chunkEnd < time - 10 || chunk.start > time + 24) {
        mixedChunkCacheRef.current.delete(chunkIndex);
      }
    }

    for (const [chunkIndex] of mixedScheduledChunksRef.current.entries()) {
      const chunkEnd = (chunkIndex + 1) * chunkSeconds;
      if (chunkEnd < time - 1) {
        stopScheduledMixedChunk(chunkIndex);
      }
    }
  };

  const scheduleMixedChunk = (chunkIndex: number, generation: number) => {
    if (generation !== mixedGenerationRef.current || mixedScheduledChunksRef.current.has(chunkIndex)) return;
    const video = videoRef.current;
    const chunk = mixedChunkCacheRef.current.get(chunkIndex);
    if (!video || !chunk || video.paused || video.ended) return;

    const ctx = ensureAudioContext();
    const gain = gainNodeRef.current;
    if (!gain || ctx.state !== "running") return;

    const playbackRate = Math.max(0.1, Math.abs(video.playbackRate || 1));
    const videoTime = video.currentTime;
    const chunkSeconds = Math.max(1, mixedAudioChunkSeconds || 8);
    const nominalChunkEnd = duration > 0
      ? Math.min(duration, chunk.start + chunkSeconds)
      : chunk.start + chunkSeconds;
    const chunkEnd = Math.min(chunk.start + chunk.buffer.duration, nominalChunkEnd);
    if (videoTime >= chunkEnd - 0.05) return;

    const offset = Math.max(0, Math.min(chunk.buffer.duration - 0.05, videoTime - chunk.start));
    const secondsUntilChunk = Math.max(0, (chunk.start - videoTime) / playbackRate);
    if (secondsUntilChunk > mixedAudioScheduleLookaheadSeconds) return;
    const sourceDuration = Math.max(0.01, chunkEnd - Math.max(videoTime, chunk.start));
    const source = ctx.createBufferSource();
    source.buffer = chunk.buffer;
    source.playbackRate.value = playbackRate;
    source.connect(gain);
    const scheduledNodes = [source];
    source.onended = () => {
      try {
        source.disconnect();
      } catch {
        // The playback generation cleanup may already have disconnected it.
      }
      // Keep the completed entry until playback passes this chunk. Removing it
      // here lets a small AudioContext/video clock skew schedule the same tail again.
    };

    mixedScheduledChunksRef.current.set(chunkIndex, scheduledNodes);
    chunk.lastUsedAt = performance.now();
    try {
      source.start(ctx.currentTime + secondsUntilChunk, offset, sourceDuration);
    } catch (error) {
      if (mixedScheduledChunksRef.current.get(chunkIndex) === scheduledNodes) {
        mixedScheduledChunksRef.current.delete(chunkIndex);
      }
      try {
        source.disconnect();
      } catch {
        // Already disconnected.
      }
      console.warn("Mixed audio schedule failed:", error);
    }
  };

  const ensureMixedBuffered = () => {
    if (!mixedPlayback || !mixedAudioChunkUrl) return;
    const video = videoRef.current;
    if (!video || video.seeking) return;

    const chunkSeconds = Math.max(1, mixedAudioChunkSeconds || 8);
    const time = Math.max(0, video.currentTime);
    const generation = mixedGenerationRef.current;
    const firstChunk = Math.max(0, Math.floor(Math.max(0, time - 0.25) / chunkSeconds));
    const lastChunk = Math.max(firstChunk, Math.floor((time + 16) / chunkSeconds));

    let prefetch = Promise.resolve();
    for (let chunkIndex = firstChunk; chunkIndex <= lastChunk; chunkIndex += 1) {
      scheduleMixedChunk(chunkIndex, generation);
      prefetch = prefetch
        .then(() => loadMixedChunk(chunkIndex, generation))
        .then(() => scheduleMixedChunk(chunkIndex, generation));
    }
    void prefetch;

    cleanupMixedChunks(time);
  };

  const restartMixedAudio = async () => {
    if (!mixedPlayback || !mixedAudioChunkUrl) return;
    stopMixedAudio();
    const ctx = ensureAudioContext();
    try {
      await ctx.resume();
    } catch (error) {
      console.warn("Mixed audio resume failed:", error);
    }
    ensureMixedBuffered();
  };

  const playMixedWhenReady = async () => {
    const video = videoRef.current;
    if (!video || !playbackRequestedRef.current) return;
    if (!mixedPlayback || !mixedAudioChunkUrl) {
      try {
        await video.play();
      } catch (error) {
        console.warn("Video play failed:", error);
      }
      return;
    }

    const requestId = mixedPlayRequestRef.current + 1;
    mixedPlayRequestRef.current = requestId;
    const generation = mixedGenerationRef.current;
    const chunkIndex = mixedChunkIndexForTime(video.currentTime);

    if (!mixedChunkCacheRef.current.has(chunkIndex)) {
      mixedPrimingPlayRef.current = true;
      if (!video.paused) video.pause();
      setPlaying(false);
      await loadMixedChunk(chunkIndex, generation);
      if (requestId !== mixedPlayRequestRef.current || generation !== mixedGenerationRef.current) {
        if (requestId === mixedPlayRequestRef.current) mixedPrimingPlayRef.current = false;
        return;
      }
      if (!playbackRequestedRef.current) {
        mixedPrimingPlayRef.current = false;
        return;
      }
    }

    const ctx = ensureAudioContext();
    try {
      await ctx.resume();
    } catch (error) {
      console.warn("Mixed audio resume failed:", error);
    }
    if (requestId !== mixedPlayRequestRef.current
      || generation !== mixedGenerationRef.current
      || !playbackRequestedRef.current) {
      if (requestId === mixedPlayRequestRef.current) mixedPrimingPlayRef.current = false;
      return;
    }

    mixedPrimingPlayRef.current = true;
    try {
      await video.play();
      ensureMixedBuffered();
    } catch (error) {
      console.warn("Video play failed:", error);
    } finally {
      if (requestId === mixedPlayRequestRef.current) mixedPrimingPlayRef.current = false;
    }
  };

  useEffect(() => {
    const video = videoRef.current;
    if (!video) return;

    if (mixedPlayback) {
      video.muted = true;
      video.volume = 0;
      if (gainNodeRef.current) gainNodeRef.current.gain.value = volume;
      return;
    }

    if (video !== connectedVideoRef.current) {
      try {
        const ctx = ensureAudioContext();
        const source = ctx.createMediaElementSource(video);
        source.connect(gainNodeRef.current!);
        connectedVideoRef.current = video;
      } catch (e) {
        console.warn("AudioContext setup failed:", e);
      }
    }

    video.muted = false;
    video.volume = Math.min(1, volume);
    if (gainNodeRef.current) {
      gainNodeRef.current.gain.value = volume > 1 ? volume : 1;
    }
  }, [volume, src, mixedPlayback]);

  useEffect(() => {
    if (gainNodeRef.current) {
      gainNodeRef.current.gain.value = mixedPlayback ? volume : volume > 1 ? volume : 1;
    }
  }, [mixedPlayback, volume]);

  useEffect(() => {
    if (!mixedPlayback || !mixedAudioChunkUrl || !src) return;

    const tick = () => ensureMixedBuffered();
    tick();
    mixedTimerRef.current = window.setInterval(tick, mixedAudioSchedulerIntervalMs);

    return () => {
      if (mixedTimerRef.current) {
        window.clearInterval(mixedTimerRef.current);
        mixedTimerRef.current = null;
      }
      clearMixedAudio();
    };
  }, [mixedPlayback, mixedAudioChunkUrl, mixedAudioChunkSeconds, src]);

  const togglePlayback = () => {
    const video = videoRef.current;
    if (!video) return;
    if (!video.paused) {
      playbackRequestedRef.current = false;
      mixedPlayRequestRef.current += 1;
      video.pause();
      if (mixedPlayback) stopMixedAudio();
      return;
    }
    playbackRequestedRef.current = true;
    void playMixedWhenReady();
  };

  const toggleFullscreen = () => {
    const shell = videoRef.current?.parentElement;
    if (document.fullscreenElement) {
      document.exitFullscreen().catch(console.error);
    } else {
      shell?.requestFullscreen().catch(console.error);
    }
  };

  const handleVideoClick = () => {
    if (fastHoldActivatedRef.current) {
      fastHoldActivatedRef.current = false;
      return;
    }
    if (videoClickTimeoutRef.current) window.clearTimeout(videoClickTimeoutRef.current);
    videoClickTimeoutRef.current = window.setTimeout(() => {
      videoClickTimeoutRef.current = null;
      togglePlayback();
    }, 220);
  };

  const handleVideoDoubleClick = () => {
    fastHoldActivatedRef.current = false;
    if (videoClickTimeoutRef.current) {
      window.clearTimeout(videoClickTimeoutRef.current);
      videoClickTimeoutRef.current = null;
    }
    toggleFullscreen();
  };

  const showControls = () => {
    setControlsVisible(true);
    if (controlsTimeoutRef.current) window.clearTimeout(controlsTimeoutRef.current);
    controlsTimeoutRef.current = window.setTimeout(() => {
      setControlsVisible(false);
    }, 2000);
  };

  const hideControls = () => {
    setControlsVisible(false);
    if (controlsTimeoutRef.current) {
      window.clearTimeout(controlsTimeoutRef.current);
      controlsTimeoutRef.current = null;
    }
  };

  useEffect(() => {
    return () => {
      if (controlsTimeoutRef.current) window.clearTimeout(controlsTimeoutRef.current);
      if (videoClickTimeoutRef.current) window.clearTimeout(videoClickTimeoutRef.current);
      clearMixedAudio();
      if (audioCtxRef.current && audioCtxRef.current.state !== "closed") {
        audioCtxRef.current.close().catch(console.error);
      }
    };
  }, []);

  const beginFastHold = () => {
    fastHoldActivatedRef.current = false;
    if (holdTimeoutRef.current) window.clearTimeout(holdTimeoutRef.current);
    holdTimeoutRef.current = window.setTimeout(() => {
      const video = videoRef.current;
      if (!video) return;
      fastHoldActivatedRef.current = true;
      video.playbackRate = 2;
      setHoldingFast(true);
    }, 500);
  };

  const endFastHold = () => {
    if (holdTimeoutRef.current) {
      window.clearTimeout(holdTimeoutRef.current);
      holdTimeoutRef.current = null;
    }
    const video = videoRef.current;
    if (video) video.playbackRate = 1;
    setHoldingFast(false);
  };

  useEffect(() => {
    const stopFastHold = () => endFastHold();
    window.addEventListener("pointerup", stopFastHold);
    window.addEventListener("blur", stopFastHold);
    document.addEventListener("visibilitychange", stopFastHold);
    return () => {
      window.removeEventListener("pointerup", stopFastHold);
      window.removeEventListener("blur", stopFastHold);
      document.removeEventListener("visibilitychange", stopFastHold);
    };
  }, []);

  const seekToPercent = (percent: number) => {
    const video = videoRef.current;
    if (!video || !Number.isFinite(video.duration)) return;
    if (mixedPlayback) resetMixedAudioTimeline();
    video.currentTime = (Math.min(100, Math.max(0, percent)) / 100) * video.duration;
  };

  const showSeekFeedback = useCallback((actualSeconds: number) => {
    if (Math.abs(actualSeconds) < 0.01) return;
    seekFeedbackTotalRef.current += actualSeconds;
    const total = seekFeedbackTotalRef.current;
    setSeekFeedback({
      direction: total < 0 ? -1 : 1,
      seconds: Math.max(1, Math.round(Math.abs(total))),
      sequence: seekFeedbackSequenceRef.current
    });
    if (seekFeedbackTimeoutRef.current) window.clearTimeout(seekFeedbackTimeoutRef.current);
    seekFeedbackTimeoutRef.current = window.setTimeout(() => {
      seekFeedbackTimeoutRef.current = null;
      setSeekFeedback(null);
      seekFeedbackTotalRef.current = 0;
    }, 850);
  }, []);

  const seekBySeconds = useCallback((seconds: number) => {
    const video = videoRef.current;
    if (!video) return;
    const videoDuration = Number.isFinite(video.duration) && video.duration > 0 ? video.duration : duration;
    if (!Number.isFinite(videoDuration) || videoDuration <= 0) return;
    const previousTime = Math.max(0, video.currentTime);
    const nextTime = Math.min(videoDuration, Math.max(0, previousTime + seconds));
    const actualSeconds = nextTime - previousTime;
    if (Math.abs(actualSeconds) < 0.01) return;
    video.currentTime = nextTime;
    setCurrentTime(nextTime);
    showSeekFeedback(actualSeconds);
  }, [duration, showSeekFeedback]);

  useEffect(() => {
    const clearKeyboardSeekTimers = () => {
      if (keyboardSeekDelayRef.current) {
        window.clearTimeout(keyboardSeekDelayRef.current);
        keyboardSeekDelayRef.current = null;
      }
      if (keyboardSeekIntervalRef.current) {
        window.clearInterval(keyboardSeekIntervalRef.current);
        keyboardSeekIntervalRef.current = null;
      }
    };

    const finishKeyboardSeek = () => {
      if (keyboardSeekDirectionRef.current === 0) return;
      clearKeyboardSeekTimers();
      keyboardSeekDirectionRef.current = 0;
      keyboardSeekingRef.current = false;
      const video = videoRef.current;
      if (!video || video.seeking) return;
      if (playbackRequestedRef.current) void playMixedWhenReady();
      else if (mixedPlayback) ensureMixedBuffered();
    };

    const handleKeyDown = (event: globalThis.KeyboardEvent) => {
      const target = event.target as HTMLElement | null;
      const interactiveTarget = target?.closest("input, textarea, select, button, a, [contenteditable='true']");
      if ((event.code === "Space" || event.key === " ") && !interactiveTarget) {
        if (event.ctrlKey || event.altKey || event.metaKey) return;
        event.preventDefault();
        if (!event.repeat) togglePlayback();
        return;
      }

      if (event.key !== "ArrowLeft" && event.key !== "ArrowRight") return;
      if (target?.closest("input, textarea, select, [contenteditable='true']")) return;
      if (event.ctrlKey || event.altKey || event.metaKey) return;
      event.preventDefault();
      if (event.repeat) return;

      finishKeyboardSeek();
      const direction: -1 | 1 = event.key === "ArrowLeft" ? -1 : 1;
      keyboardSeekDirectionRef.current = direction;
      keyboardSeekStartedAtRef.current = performance.now();
      keyboardSeekingRef.current = true;
      seekFeedbackTotalRef.current = 0;
      seekFeedbackSequenceRef.current += 1;
      setSeekFeedback(null);
      seekBySeconds(direction * 5);

      keyboardSeekDelayRef.current = window.setTimeout(() => {
        const seekAgain = () => {
          const heldMs = performance.now() - keyboardSeekStartedAtRef.current;
          const step = heldMs >= 2000 ? 20 : heldMs >= 900 ? 10 : 5;
          seekBySeconds(direction * step);
        };
        seekAgain();
        keyboardSeekIntervalRef.current = window.setInterval(seekAgain, 120);
      }, 300);
    };

    const handleKeyUp = (event: globalThis.KeyboardEvent) => {
      const direction = event.key === "ArrowLeft" ? -1 : event.key === "ArrowRight" ? 1 : 0;
      if (direction !== 0 && direction === keyboardSeekDirectionRef.current) finishKeyboardSeek();
    };

    const handleVisibilityChange = () => {
      if (document.hidden) finishKeyboardSeek();
    };

    window.addEventListener("keydown", handleKeyDown);
    window.addEventListener("keyup", handleKeyUp);
    window.addEventListener("blur", finishKeyboardSeek);
    document.addEventListener("visibilitychange", handleVisibilityChange);
    return () => {
      clearKeyboardSeekTimers();
      keyboardSeekDirectionRef.current = 0;
      keyboardSeekingRef.current = false;
      if (seekFeedbackTimeoutRef.current) {
        window.clearTimeout(seekFeedbackTimeoutRef.current);
        seekFeedbackTimeoutRef.current = null;
      }
      window.removeEventListener("keydown", handleKeyDown);
      window.removeEventListener("keyup", handleKeyUp);
      window.removeEventListener("blur", finishKeyboardSeek);
      document.removeEventListener("visibilitychange", handleVisibilityChange);
    };
  }, [mixedPlayback, seekBySeconds]);

  const progressPercent = duration > 0 ? Math.min(100, Math.max(0, (currentTime / duration) * 100)) : 0;

  const aspectWidth = parseInt((clip.resolution || "").split("x")[0]) || 16;
  const aspectHeight = parseInt((clip.resolution || "").split("x")[1]) || 9;

  return (
    <section className={embeddedInLibrary ? "player panel library-player" : "player panel"}>
      <div className={embeddedInLibrary ? "library-player-main" : "player-content"}>
        {!embeddedInLibrary && (
          <div className="player-header">
            <div>
              <strong className="player-title">
                {iconUrl && <img className="clip-app-icon" src={iconUrl} alt="" />}
                <span>{displayTitle}</span>
              </strong>
              <span>
                {sourceText}
              </span>
            </div>
            <button className="icon-button" title="Close player" onClick={onClose}>
              <X size={16} />
            </button>
          </div>
        )}
        {src ? (
        <div
          className={`custom-video-shell ${controlsVisible ? "" : "controls-hidden"}`}
          style={embeddedInLibrary
            ? { maxWidth: "none" }
            : { maxWidth: `calc(480px * (${aspectWidth} / ${aspectHeight}))` }}
          onPointerMove={showControls}
          onPointerEnter={showControls}
          onPointerDown={(event) => {
            if ((event.target as HTMLElement).closest(".player-controls")) return;
            if (event.button !== 0) return;
            beginFastHold();
          }}
          onPointerUp={endFastHold}
          onPointerCancel={() => {
            endFastHold();
            hideControls();
          }}
          onPointerLeave={() => {
            endFastHold();
            hideControls();
          }}
        >
          <video
            ref={videoRef}
            key={src}
            src={src}
            autoPlay={!mixedPlayback}
            crossOrigin="anonymous"
            muted={mixedPlayback}
            preload="metadata"
            onClick={handleVideoClick}
            onDoubleClick={handleVideoDoubleClick}
            onPlay={() => {
              setPlaying(true);
              if (!mixedPlayback) return;
              if (mixedPrimingPlayRef.current) {
                stopMixedAudio();
                ensureMixedBuffered();
                return;
              }
              const chunkIndex = mixedChunkIndexForTime(videoRef.current?.currentTime ?? 0);
              if (!mixedChunkCacheRef.current.has(chunkIndex)) {
                videoRef.current?.pause();
                setPlaying(false);
                void playMixedWhenReady();
                return;
              }
              void restartMixedAudio();
            }}
            onPause={() => {
              setPlaying(false);
              if (mixedPlayback) stopMixedAudio();
            }}
            onLoadedMetadata={(event) => {
              const nextDuration = event.currentTarget.duration;
              if (Number.isFinite(nextDuration)) setDuration(nextDuration);
              if (mixedPlayback) {
                ensureMixedBuffered();
                void playMixedWhenReady();
              }
            }}
            onTimeUpdate={(event) => {
              setCurrentTime(event.currentTarget.currentTime);
              if (mixedPlayback) ensureMixedBuffered();
            }}
            onSeeking={() => {
              if (mixedPlayback) {
                mixedPlayRequestRef.current += 1;
                resetMixedAudioTimeline();
              }
            }}
            onSeeked={(event) => {
              setCurrentTime(event.currentTarget.currentTime);
              if (keyboardSeekingRef.current) return;
              if (playbackRequestedRef.current) void playMixedWhenReady();
              else if (mixedPlayback) ensureMixedBuffered();
            }}
            onRateChange={(event) => {
              if (mixedPlayback && !event.currentTarget.paused) void restartMixedAudio();
            }}
            onEnded={() => {
              playbackRequestedRef.current = false;
              setPlaying(false);
              if (mixedPlayback) stopMixedAudio();
              endFastHold();
            }}
          />
          {holdingFast && <div className="speed-pill">2x</div>}
          {seekFeedback && (
            <div key={seekFeedback.sequence} className={`seek-feedback ${seekFeedback.direction < 0 ? "backward" : "forward"}`} aria-hidden="true">
              {seekFeedback.direction < 0 && <ChevronLeft className="seek-feedback-chevron" />}
              {seekFeedback.direction < 0
                ? <Minus className="seek-feedback-sign" />
                : <Plus className="seek-feedback-sign" />}
              <strong className="seek-feedback-number">{seekFeedback.seconds}</strong>
              {seekFeedback.direction > 0 && <ChevronRight className="seek-feedback-chevron" />}
            </div>
          )}
          <div className="player-controls">
            <input
              className="player-scrubber"
              type="range"
              min={0}
              max={100}
              step={0.1}
              value={progressPercent}
              onChange={(event) => seekToPercent(Number(event.target.value))}
              aria-label="Seek"
            />
            <div className="player-controls-bottom">
              <div className="player-controls-left">
                <button className="yt-pill icon-button" title={playing ? "Pause" : "Play"} onClick={togglePlayback}>
                  {playing ? <Pause size={24} /> : <Play size={24} />}
                </button>
                <div className="yt-pill volume-container">
                  <Volume2 size={22} />
                  <input
                    className="volume-slider"
                    type="range"
                    min={0}
                    max={5.62}
                    step={0.05}
                    value={volume}
                    onChange={(event) => setVolume(Number(event.target.value))}
                    aria-label="Volume"
                  />
                </div>
                <div className="yt-pill time-readout">
                  {formatDuration(currentTime)} / {formatDuration(duration)}
                </div>
              </div>
              <div className="player-controls-right">
                <button className="yt-pill icon-button" title="Fullscreen" onClick={toggleFullscreen}>
                  <Maximize2 size={22} />
                </button>
              </div>
            </div>
          </div>
        </div>
        ) : <div className="empty">{message}</div>}
        {!embeddedInLibrary && (
          <div className="player-meta">
            <span>{formatDuration(clip.durationSeconds)}</span>
            <span>{clip.resolution}</span>
            <span>{clip.fps} FPS</span>
            <span>{displayAudioTracks(clip.audioTracks) || "No audio tracks"}</span>
            <span>{message}</span>
          </div>
        )}
      </div>
      {embeddedInLibrary && railClips && onSelectClip && (
        <LibraryPlayerSidebar
          clip={clip}
          railClips={railClips}
          settings={settings}
          onSelectClip={onSelectClip}
          onClose={onClose}
        />
      )}
    </section>
  );
}

function SystemAudioModal({
  source,
  activeProcesses,
  otherAppProcesses,
  onSave,
  onClose
}: {
  source: AudioSourceRule;
  activeProcesses: ActiveProcess[];
  otherAppProcesses: Set<string>;
  onSave: (patch: Partial<AudioSourceRule>) => void;
  onClose: () => void;
}) {
  const [captureAll, setCaptureAll] = useState(source.captureAllSystem ?? true);
  const [selectedProcesses, setSelectedProcesses] = useState<Set<string>>(
    new Set(source.processNames ?? [])
  );
  const [search, setSearch] = useState("");

  const handleToggle = (name: string) => {
    const next = new Set(selectedProcesses);
    if (next.has(name)) next.delete(name);
    else next.add(name);
    setSelectedProcesses(next);
  };

  const filteredProcesses = activeProcesses.filter((p) =>
    p.name.toLowerCase().includes(search.toLowerCase())
  );

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal system-audio-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <h2>Configure System Audio Mix</h2>
          <button className="icon-button" onClick={onClose}>
            <X size={18} />
          </button>
        </div>
        <div className="modal-body">
          <label className="radio-label">
            <input
              type="radio"
              checked={captureAll}
              onChange={() => setCaptureAll(true)}
            />
            <span>Record entire system (All apps)</span>
          </label>
          <label className="radio-label">
            <input
              type="radio"
              checked={!captureAll}
              onChange={() => setCaptureAll(false)}
            />
            <span>Record specific apps only</span>
          </label>

          {!captureAll && (
            <div className="process-list-container">
              <div style={{ display: "flex", gap: "8px" }}>
                <input
                  className="process-search"
                  value={search}
                  onChange={(e) => setSearch(e.target.value)}
                  placeholder="Search processes..."
                  style={{ flex: 1 }}
                />
                <button
                  className="secondary-button"
                  onClick={() => {
                    const visibleNames = Array.from(new Set([
                      ...filteredProcesses.map(p => p.name),
                      ...Array.from(selectedProcesses)
                    ])).filter((name) => name.toLowerCase().includes(search.toLowerCase()));
                    
                    const validProcesses = visibleNames.filter((name) => !otherAppProcesses.has(name));
                    const anySelected = validProcesses.some((name) => selectedProcesses.has(name));
                    
                    const next = new Set(selectedProcesses);
                    if (anySelected) {
                      validProcesses.forEach((name) => next.delete(name));
                    } else {
                      validProcesses.forEach((name) => next.add(name));
                    }
                    setSelectedProcesses(next);
                  }}
                >
                  {(() => {
                    const visibleNames = Array.from(new Set([
                      ...filteredProcesses.map(p => p.name),
                      ...Array.from(selectedProcesses)
                    ])).filter((name) => name.toLowerCase().includes(search.toLowerCase()));
                    return visibleNames.filter((name) => !otherAppProcesses.has(name)).some((name) => selectedProcesses.has(name)) 
                      ? "Clear All" 
                      : "Select All";
                  })()}
                </button>
              </div>
              <div className="process-list">
                {Array.from(new Set([...filteredProcesses.map(p => p.name), ...Array.from(selectedProcesses)]))
                  .filter((name) => name.toLowerCase().includes(search.toLowerCase()))
                  .sort((a, b) => {
                    const aSelected = selectedProcesses.has(a);
                    const bSelected = selectedProcesses.has(b);
                    if (aSelected && !bSelected) return -1;
                    if (!aSelected && bSelected) return 1;
                    return a.localeCompare(b);
                  })
                  .map((name) => {
                  const isSeparateTrack = otherAppProcesses.has(name);
                  const isOffline = !activeProcesses.some(p => p.name === name);
                  return (
                    <label
                      key={name}
                      className={`process-item ${isSeparateTrack ? "disabled" : ""}`}
                      title={isSeparateTrack ? "This app is already configured as a separate track." : ""}
                    >
                      <input
                        className="toggle-switch"
                        type="checkbox"
                        checked={selectedProcesses.has(name)}
                        disabled={isSeparateTrack}
                        onChange={() => handleToggle(name)}
                      />
                      <span>{name} {isOffline && <span style={{opacity: 0.5}}>(Offline)</span>}</span>
                      {isSeparateTrack && <span className="badge">Separate Track</span>}
                    </label>
                  );
                })}
              </div>
            </div>
          )}
        </div>
        <div className="modal-footer">
          <button onClick={onClose}>Cancel</button>
          <button
            className="primary"
            onClick={() => {
              onSave({
                captureAllSystem: captureAll,
                processNames: Array.from(selectedProcesses)
              });
              onClose();
            }}
          >
            Save Configuration
          </button>
        </div>
      </div>
    </div>
  );
}

function AppAudioModal({
  source,
  activeProcesses,
  onSave,
  onClose
}: {
  source: AudioSourceRule;
  activeProcesses: ActiveProcess[];
  onSave: (patch: Partial<AudioSourceRule>) => void;
  onClose: () => void;
}) {
  const [search, setSearch] = useState("");

  const filteredProcesses = activeProcesses.filter((p) =>
    p.name.toLowerCase().includes(search.toLowerCase())
  );

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal system-audio-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <h2>Select App Process</h2>
          <button className="icon-button" onClick={onClose}>
            <X size={18} />
          </button>
        </div>
        <div className="modal-body">
          <input
            className="process-search"
            value={search}
            onChange={(e) => setSearch(e.target.value)}
            placeholder="Search processes..."
            autoFocus
          />
          <div className="process-list" style={{ marginTop: "12px", maxHeight: "300px", overflowY: "auto" }}>
            {filteredProcesses.map((p) => (
              <label key={`${p.name}-${p.pid}`} className="process-item">
                <input
                  type="radio"
                  name="app-process"
                  checked={source.processName === p.name}
                  onChange={() => {
                    onSave({
                      label: p.name || "App audio",
                      processName: p.name,
                      executablePath: p.executablePath,
                      enabled: true
                    });
                  }}
                />
                <span>{p.name}</span>
              </label>
            ))}
          </div>
        </div>
      </div>
    </div>
  );
}

function AppAudioSourceIcon({ source, fallback }: { source: AudioSourceRule; fallback: string }) {
  const [iconUrl, setIconUrl] = useState("");
  const processName = source.processName || "";
  const executablePath = source.executablePath || "";

  useEffect(() => {
    let active = true;
    setIconUrl("");
    if (!processName) {
      return () => {
        active = false;
      };
    }

    window.clipture.processIconUrl(processName, executablePath).then((url) => {
      if (active) setIconUrl(url || "");
    }).catch(() => {
      if (active) setIconUrl("");
    });

    return () => {
      active = false;
    };
  }, [processName, executablePath]);

  return (
    <span className={iconUrl ? "audio-source-icon audio-app-icon has-image" : "audio-source-icon audio-app-icon"} aria-hidden="true">
      {iconUrl ? <img className="audio-app-icon-image" src={iconUrl} alt="" /> : fallback}
    </span>
  );
}

const maxManualNoiseGateThreshold = 0.2;
const visualizerMinDb = -60;
const visualizerMaxDb = 20 * Math.log10(maxManualNoiseGateThreshold);
const minMicGainDb = -60;
const maxMicGainDb = 25;

function visualizerLevelFromRms(rms: number) {
  if (rms <= 0) return 0;
  const db = 20 * Math.log10(rms);
  return Math.min(1, Math.max(0, (db - visualizerMinDb) / (visualizerMaxDb - visualizerMinDb)));
}

function rmsFromVisualizerLevel(level: number) {
  const boundedLevel = Math.min(1, Math.max(0, level));
  const db = visualizerMinDb + boundedLevel * (visualizerMaxDb - visualizerMinDb);
  return Math.pow(10, db / 20);
}

function gainToDb(gain: number) {
  if (gain <= 0) return minMicGainDb;
  return Math.min(maxMicGainDb, Math.max(minMicGainDb, Math.round(20 * Math.log10(gain) * 2) / 2));
}

function dbToGain(db: number) {
  if (db <= minMicGainDb) return 0;
  return Number(Math.pow(10, db / 20).toFixed(4));
}

function formatDb(db: number) {
  if (db <= minMicGainDb) return "-inf dB";
  if (db > 0) return `+${db} dB`;
  return `${db} dB`;
}

function sensitivityFromThreshold(threshold: number) {
  const visualizerLevel = visualizerLevelFromRms(threshold);
  return Math.round((1 - visualizerLevel) * 100 * 2) / 2;
}

function thresholdFromSensitivity(sensitivity: number) {
  const boundedSensitivity = Math.min(100, Math.max(0, sensitivity));
  return Number(rmsFromVisualizerLevel(1 - boundedSensitivity / 100).toFixed(4));
}

function MicrophoneSettingsModal({
  source,
  inputDevices,
  onUpdate,
  onClose
}: {
  source: AudioSourceRule;
  inputDevices: AudioInputDevice[];
  onUpdate: (patch: Partial<AudioSourceRule>) => void;
  onClose: () => void;
}) {
  const volume = source.volume ?? 1.0;
  const volumeDb = gainToDb(volume);
  const voiceIsolation = source.voiceIsolation ?? false;
  const voiceIsolationWeight = source.voiceIsolationWeight ?? 1.0;
  const noiseGateEnabled = source.noiseGateEnabled ?? true;
  const autoNoiseGate = source.autoNoiseGate ?? true;
  const noiseGateThreshold = source.noiseGateThreshold ?? 0.05;
  const noiseGateDebounceMs = source.noiseGateDebounceMs ?? 180;
  const micDeviceId = source.micDeviceId ?? "";
  const micDeviceMatchKey = source.micDeviceMatchKey ?? "";
  const micDeviceName = source.micDeviceName ?? "";
  const visibleInputDevices = [...inputDevices];
  if (micDeviceId && !visibleInputDevices.some((device) => device.id === micDeviceId)) {
    visibleInputDevices.unshift({
      id: micDeviceId,
      name: micDeviceName || "Selected microphone",
      isDefault: false,
      state: "unavailable",
      matchKey: micDeviceMatchKey
    });
  }
  const manualSensitivity = sensitivityFromThreshold(noiseGateThreshold);

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <h2>Microphone Settings</h2>
          <button className="icon-button" onClick={onClose}>
            <X size={18} />
          </button>
        </div>
        <div className="modal-body" style={{ gap: "16px" }}>
        <label>
          Input device
          <select
            value={micDeviceId}
            onChange={(event) => {
              const selectedId = event.target.value;
              const selectedDevice = visibleInputDevices.find((device) => device.id === selectedId);
              onUpdate({
                micDeviceId: selectedId,
                micDeviceMatchKey: selectedDevice?.matchKey ?? "",
                micDeviceName: selectedDevice?.name ?? ""
              });
            }}
          >
            <option value="">System default</option>
            {visibleInputDevices.map((device) => (
              <option key={device.id} value={device.id}>
                {device.name}{device.state === "unavailable" ? " (unplugged, using default)" : device.isDefault ? " (default)" : ""}
              </option>
            ))}
          </select>
        </label>

        <label>
          Mic gain
          <div className="range-line">
            <input
              type="range"
              min={minMicGainDb}
              max={maxMicGainDb}
              step={0.5}
              value={volumeDb}
              onChange={(event) => onUpdate({ volume: dbToGain(Number(event.target.value)) })}
            />
            <span>{formatDb(volumeDb)}</span>
          </div>
        </label>

        <label>
          Voice isolation
          <select
            value={voiceIsolation ? "on" : "off"}
            onChange={(event) => onUpdate({ voiceIsolation: event.target.value === "on" })}
          >
            <option value="off">Off</option>
            <option value="on">On</option>
          </select>
        </label>

        {voiceIsolation && (
          <label>
            Isolation strength
            <div className="range-line">
              <input
                type="range"
                min={0}
                max={1}
                step={0.05}
                value={voiceIsolationWeight}
                onChange={(event) => onUpdate({ voiceIsolationWeight: parseFloat(event.target.value) })}
              />
              <span>{Math.round(voiceIsolationWeight * 100)}%</span>
            </div>
          </label>
        )}

        <label>
          Input sensitivity
          <select
            value={!noiseGateEnabled ? "off" : autoNoiseGate ? "auto" : "manual"}
            onChange={(event) => {
              const mode = event.target.value;
              onUpdate({
                noiseGateEnabled: mode !== "off",
                autoNoiseGate: mode === "auto",
                noiseGateThreshold
              });
            }}
          >
            <option value="off">Off</option>
            <option value="auto">Auto</option>
            <option value="manual">Manual</option>
          </select>
        </label>

        {noiseGateEnabled && !autoNoiseGate && (
          <label>
            Manual sensitivity
            <div className="range-line">
              <input
                type="range"
                min={0}
                max={100}
                step={0.5}
                value={manualSensitivity}
                onChange={(event) => onUpdate({ noiseGateThreshold: thresholdFromSensitivity(Number(event.target.value)) })}
              />
              <span>{manualSensitivity}%</span>
            </div>
          </label>
        )}

        {noiseGateEnabled && (
          <label>
            Debounce time
            <div className="range-line">
              <input
                type="range"
                min={0}
                max={1000}
                step={20}
                value={noiseGateDebounceMs}
                onChange={(event) => onUpdate({ noiseGateDebounceMs: Number(event.target.value) })}
              />
              <span>{noiseGateDebounceMs}ms</span>
            </div>
          </label>
        )}

        <div className="mic-test-row">
          <TestMicButton
            volume={volume}
            voiceIsolation={voiceIsolation}
            voiceIsolationWeight={voiceIsolationWeight}
            noiseGateEnabled={noiseGateEnabled}
            autoNoiseGate={autoNoiseGate}
            noiseGateThreshold={noiseGateThreshold}
            noiseGateDebounceMs={noiseGateDebounceMs}
          />
        </div>
        </div>
      </div>
    </div>
  );
}

function DraftNumberInput({
  value,
  min,
  max,
  disabled = false,
  onCommit
}: {
  value: number;
  min: number;
  max: number;
  disabled?: boolean;
  onCommit: (value: number) => void;
}) {
  const [draft, setDraft] = useState(() => String(value));
  const cancelNextBlur = useRef(false);

  useEffect(() => {
    setDraft(String(value));
  }, [value]);

  const commitDraft = () => {
    const parsed = Number(draft.trim());
    if (!draft.trim() || !Number.isFinite(parsed)) {
      setDraft(String(value));
      return;
    }
    const nextValue = Math.min(max, Math.max(min, Math.round(parsed)));
    setDraft(String(nextValue));
    if (nextValue !== value) onCommit(nextValue);
  };

  return (
    <input
      type="text"
      inputMode="numeric"
      pattern="[0-9]*"
      value={draft}
      disabled={disabled}
      onChange={(event) => {
        if (/^\d*$/.test(event.target.value)) setDraft(event.target.value);
      }}
      onBlur={() => {
        if (cancelNextBlur.current) {
          cancelNextBlur.current = false;
          setDraft(String(value));
          return;
        }
        commitDraft();
      }}
      onKeyDown={(event) => {
        if (event.key === "Enter") {
          event.currentTarget.blur();
        } else if (event.key === "Escape") {
          cancelNextBlur.current = true;
          setDraft(String(value));
          event.currentTarget.blur();
        }
      }}
    />
  );
}

function ThemeColorField({
  label,
  value,
  onPreview,
  onCommit
}: {
  label: string;
  value: string;
  onPreview: (value: string) => void;
  onCommit: (value: string) => void;
}) {
  const [draft, setDraft] = useState(value);
  const [editing, setEditing] = useState(false);
  const cancelCommit = useRef(false);

  useEffect(() => {
    if (!editing) setDraft(value);
  }, [editing, value]);

  const commit = (candidate: string) => {
    const withHash = candidate.startsWith("#") ? candidate : `#${candidate}`;
    const normalized = normalizeThemeColor(withHash, value);
    setDraft(normalized);
    onPreview(normalized);
    if (normalized !== value) onCommit(normalized);
  };

  return (
    <label className="theme-color-field">
      <span>{label}</span>
      <span className="theme-color-control">
        <input
          aria-label={`Choose ${label.toLowerCase()}`}
          className="theme-color-picker"
          type="color"
          value={normalizeThemeColor(draft, value)}
          onChange={(event) => {
            const color = event.currentTarget.value.toLowerCase();
            setDraft(color);
            onPreview(color);
          }}
          onBlur={(event) => commit(event.currentTarget.value)}
        />
        <input
          aria-label={`${label} hex value`}
          className="theme-color-hex"
          maxLength={7}
          spellCheck={false}
          value={draft}
          onFocus={() => setEditing(true)}
          onChange={(event) => {
            const next = event.currentTarget.value;
            if (!/^#?[0-9a-f]{0,6}$/i.test(next)) return;
            setDraft(next);
            const withHash = next.startsWith("#") ? next : `#${next}`;
            if (/^#[0-9a-f]{6}$/i.test(withHash)) onPreview(withHash.toLowerCase());
          }}
          onBlur={() => {
            setEditing(false);
            if (cancelCommit.current) {
              cancelCommit.current = false;
              return;
            }
            commit(draft);
          }}
          onKeyDown={(event) => {
            if (event.key === "Enter") event.currentTarget.blur();
            if (event.key === "Escape") {
              cancelCommit.current = true;
              setDraft(value);
              onPreview(value);
              event.currentTarget.blur();
            }
          }}
        />
      </span>
    </label>
  );
}

function CustomizeSettings({ settings, onChange }: { settings: ClipSettings; onChange: (patch: Partial<ClipSettings>) => void }) {
  const [mainColor, setMainColor] = useState(settings.customMainColor);
  const [accentColor, setAccentColor] = useState(settings.customAccentColor);
  const [themeFontAvailable, setThemeFontAvailable] = useState<boolean>();
  const themes = [
    { id: "graphite", label: "Graphite", Icon: Moon },
    { id: "light", label: "Light", Icon: Sun },
    { id: "glitten", label: "Glitten", Icon: Feather },
    { id: "milate", label: "Milate", Icon: Leaf },
    { id: "custom", label: "Custom", Icon: Palette }
  ] as const;
  const selectedThemeFont: ThemeFontId | undefined = settings.uiTheme === "glitten" || settings.uiTheme === "milate"
    ? settings.uiTheme
    : undefined;
  const selectedThemeFontLabel = selectedThemeFont === "glitten" ? "Glitten" : "Milate";

  useEffect(() => setMainColor(settings.customMainColor), [settings.customMainColor]);
  useEffect(() => setAccentColor(settings.customAccentColor), [settings.customAccentColor]);

  useEffect(() => {
    if (!selectedThemeFont) {
      setThemeFontAvailable(undefined);
      return;
    }

    let active = true;
    const refresh = () => {
      setThemeFontAvailable(undefined);
      void refreshLocalThemeFont(selectedThemeFont).then((available) => {
        if (active) setThemeFontAvailable(available);
      });
    };

    refresh();
    window.addEventListener("focus", refresh);
    return () => {
      active = false;
      window.removeEventListener("focus", refresh);
    };
  }, [selectedThemeFont]);

  const previewCustom = (nextMain: string, nextAccent: string) => {
    setMainColor(nextMain);
    setAccentColor(nextAccent);
    applyUiTheme({ uiTheme: "custom", customMainColor: nextMain, customAccentColor: nextAccent });
  };

  const customPreviewStyle = {
    "--preview-main": mainColor,
    "--preview-accent": accentColor
  } as CSSProperties;

  return (
    <div className="settings-group single-column customize-settings-group">
      <div className="customize-settings-panel">
        <div className="audio-settings-heading">
          <h2>Appearance</h2>
        </div>

        <div className="theme-options" role="radiogroup" aria-label="Interface theme">
          {themes.map(({ id, label, Icon }) => (
            <button
              aria-checked={settings.uiTheme === id}
              className={settings.uiTheme === id ? "theme-option selected" : "theme-option"}
              key={id}
              onClick={() => onChange({ uiTheme: id })}
              role="radio"
              type="button"
            >
              <span className={`theme-preview ${id}`} style={id === "custom" ? customPreviewStyle : undefined} aria-hidden="true">
                <span className="theme-preview-sidebar" />
                <span className="theme-preview-content">
                  <span className="theme-preview-line" />
                  <span className="theme-preview-button" />
                </span>
              </span>
              <span className="theme-option-label"><Icon size={17} /> {label}</span>
              {settings.uiTheme === id && <Check className="theme-option-check" size={17} aria-hidden="true" />}
            </button>
          ))}
        </div>

        {selectedThemeFont && (
          <div className="theme-font-tools">
            <div className="theme-font-status">
              <strong>{selectedThemeFontLabel} typeface</strong>
              <span>{themeFontAvailable === undefined ? "Checking..." : themeFontAvailable ? "Installed" : "Fallback active"}</span>
            </div>
            <div className="theme-font-actions">
              <button className="secondary-button" type="button" onClick={() => void window.clipture.openThemeFontDownload(selectedThemeFont)}>
                <ExternalLink size={16} /> Get {selectedThemeFontLabel}
              </button>
              <button
                className="secondary-button"
                type="button"
                onClick={() => {
                  setThemeFontAvailable(undefined);
                  void refreshLocalThemeFont(selectedThemeFont).then(setThemeFontAvailable);
                }}
              >
                <RefreshCw size={16} /> Refresh font
              </button>
            </div>
          </div>
        )}

        {settings.uiTheme === "custom" && (
          <div className="custom-colors">
            <h3>Custom colors</h3>
            <div className="theme-color-grid">
              <ThemeColorField
                label="Main color"
                value={settings.customMainColor}
                onPreview={(color) => previewCustom(color, accentColor)}
                onCommit={(customMainColor) => onChange({ customMainColor })}
              />
              <ThemeColorField
                label="Accent color"
                value={settings.customAccentColor}
                onPreview={(color) => previewCustom(mainColor, color)}
                onCommit={(customAccentColor) => onChange({ customAccentColor })}
              />
            </div>
          </div>
        )}
      </div>
    </div>
  );
}

function SettingsView({
  settings,
  clipSounds,
  onChange,
  onPreviewSound,
  onImportSound,
  onRevealSounds
}: {
  settings: ClipSettings;
  clipSounds: ClipSoundOption[];
  onChange: (patch: Partial<ClipSettings>) => void;
  onPreviewSound: (sound: string) => void;
  onImportSound: () => void;
  onRevealSounds: () => void;
}) {
  const audioSources = settings.audioSources || [];
  const builtInAudioSources = audioSources.filter((source) => source.kind !== "app");
  const appAudioSources = audioSources.filter((source) => source.kind === "app");
  const [recordingHotkey, setRecordingHotkey] = useState(false);
  const [activeProcesses, setActiveProcesses] = useState<ActiveProcess[]>([]);
  const [inputDevices, setInputDevices] = useState<AudioInputDevice[]>([]);
  const [displayDevices, setDisplayDevices] = useState<DisplayDevice[]>([]);
  const [configuringSystemAudio, setConfiguringSystemAudio] = useState(false);
  const [configuringAppAudio, setConfiguringAppAudio] = useState<string | null>(null);
  const [configuringMicAudio, setConfiguringMicAudio] = useState<string | null>(null);
  const [activeCategory, setActiveCategory] = useState<"general" | "video" | "audio" | "customize">("general");
  const [editingClipLength, setEditingClipLength] = useState(false);
  const [clipLengthDraft, setClipLengthDraft] = useState(() => String(settings.clipLengthSeconds));

  useEffect(() => {
    void window.clipture.listActiveProcesses().then(setActiveProcesses);
    void window.clipture.listAudioInputDevices().then(setInputDevices);
    void window.clipture.listDisplayDevices().then(setDisplayDevices);
  }, []);

  useEffect(() => {
    if (!editingClipLength) setClipLengthDraft(String(settings.clipLengthSeconds));
  }, [editingClipLength, settings.clipLengthSeconds]);

  const addSource = () => {
    const id = `app-${Date.now()}`;
    void onChange({
      audioSources: [
        ...audioSources,
        {
          id,
          kind: "app",
          label: "New App Source",
          enabled: true,
          omitIfSilent: true
        }
      ]
    });
    void window.clipture.listActiveProcesses().then(setActiveProcesses);
    setConfiguringAppAudio(id);
  };

  const removeSource = (sourceId: string) => {
    const source = settings.audioSources.find((candidate) => candidate.id === sourceId);
    if (!source || source.kind !== "app") return;
    if (configuringAppAudio === sourceId) setConfiguringAppAudio(null);
    void onChange({
      audioSources: settings.audioSources.filter((candidate) => candidate.id !== sourceId)
    });
  };

  const otherAppProcesses = new Set(
    settings.audioSources
      .filter((s) => s.kind === "app" && s.enabled && s.processName)
      .map((s) => s.processName!)
  );

  const updateAudioSource = (sourceId: string, patch: Partial<AudioSourceRule>) => {
    onChange({
      audioSources: settings.audioSources.map((candidate) =>
        candidate.id === sourceId ? { ...candidate, ...patch } : candidate
      )
    });
  };

  const setAudioSourceEnabled = (sourceId: string, enabled: boolean) => {
    updateAudioSource(sourceId, { enabled });
  };

  const audioSourceIcon = (source: AudioSourceRule) => {
    switch (source.kind) {
      case "system":
        return <Volume2 size={20} />;
      case "microphone":
        return <Mic size={20} />;
      case "game":
        return <Gamepad2 size={20} />;
      default:
        return <Volume2 size={20} />;
    }
  };

  const audioSourceDescription = (source: AudioSourceRule) => {
    switch (source.kind) {
      case "system":
        return source.captureAllSystem === false ? "Record selected apps into the system mix" : "Record all system sounds";
      case "microphone":
        return source.micDeviceName ? `Record from ${source.micDeviceName}` : "Record from your microphone";
      case "game":
        return "Record audio from the active game or app";
      default:
        return source.enabled ? "Recorded when enabled" : "Currently disabled";
    }
  };

  const appSourceName = (source: AudioSourceRule) => source.processName || source.label || "Select process";

  const appSourceInitial = (source: AudioSourceRule) => {
    const name = appSourceName(source).replace(/\.exe$/i, "").trim();
    return name ? name[0]!.toUpperCase() : "A";
  };

  const commitClipLengthDraft = () => {
    const trimmed = clipLengthDraft.trim();
    if (!trimmed) {
      setClipLengthDraft(String(settings.clipLengthSeconds));
      return;
    }
    const parsed = Number(trimmed);
    if (!Number.isFinite(parsed)) {
      setClipLengthDraft(String(settings.clipLengthSeconds));
      return;
    }
    const nextLength = Math.min(600, Math.max(5, Math.round(parsed)));
    setClipLengthDraft(String(nextLength));
    if (nextLength !== settings.clipLengthSeconds) {
      onChange({ clipLengthSeconds: nextLength });
    }
  };

  useEffect(() => {
    const micSource = settings.audioSources.find((source) => source.kind === "microphone");
    if (!micSource?.micDeviceId) return;
    const activeDevice = inputDevices.find((device) => device.id === micSource.micDeviceId);
    if (!activeDevice) return;
    const nextMatchKey = activeDevice.matchKey ?? "";
    if (micSource.micDeviceMatchKey === nextMatchKey && micSource.micDeviceName === activeDevice.name) return;
    updateAudioSource(micSource.id, {
      micDeviceMatchKey: nextMatchKey,
      micDeviceName: activeDevice.name
    });
  }, [inputDevices, settings.audioSources]);

  return (
    <section className="settings-container">
      <div className="settings-tabs">
        <button className={activeCategory === 'general' ? 'settings-tab active' : 'settings-tab'} onClick={() => setActiveCategory('general')}>General</button>
        <button className={activeCategory === 'video' ? 'settings-tab active' : 'settings-tab'} onClick={() => setActiveCategory('video')}>Video</button>
        <button className={activeCategory === 'audio' ? 'settings-tab active' : 'settings-tab'} onClick={() => setActiveCategory('audio')}>Audio</button>
        <button className={activeCategory === 'customize' ? 'settings-tab active' : 'settings-tab'} onClick={() => setActiveCategory('customize')}><Paintbrush size={16} /> Customize</button>
      </div>

      <div className="settings-content">
        {activeCategory === 'general' && (
          <div className="settings-group">
      <label>
        Hotkey
        <button
          className={recordingHotkey ? "keybind-button recording" : "keybind-button"}
          onBlur={() => setRecordingHotkey(false)}
          onClick={(event) => {
            setRecordingHotkey(true);
            event.currentTarget.focus();
          }}
          onKeyDown={(event) => {
            event.preventDefault();
            event.stopPropagation();
            if (event.key === "Escape") {
              setRecordingHotkey(false);
              return;
            }
            if (event.key === "Backspace" || event.key === "Delete") {
              void onChange({ hotkey: "" });
              setRecordingHotkey(false);
              return;
            }
            const accelerator = acceleratorFromKeyboardEvent(event);
            if (!accelerator) return;
            void onChange({ hotkey: accelerator });
            setRecordingHotkey(false);
          }}
          type="button"
        >
          {recordingHotkey ? "Press shortcut" : settings.hotkey || "Unassigned"}
        </button>
      </label>
      <label className="wide">
        Save folder
        <input
          readOnly
          style={{ cursor: "pointer" }}
          title="Click to select a new folder"
          value={settings.saveFolder}
          onClick={async () => {
            const folder = await window.clipture.selectFolder(settings.saveFolder);
            if (folder && folder !== settings.saveFolder) {
              void onChange({ saveFolder: folder });
            }
          }}
        />
      </label>
      <label className="wide">
        Clip sound cue
        <select
          value={settings.clipSound || 'none'}
          onChange={(event) => {
            const sound = event.target.value;
            onChange({ clipSound: sound });
            onPreviewSound(sound);
          }}
        >
          <option value="none">None</option>
          {clipSounds.map((sound) => (
            <option key={sound.id} value={sound.id}>
              {sound.label}
            </option>
          ))}
        </select>
      </label>
      <div className="sound-actions wide">
        <button className="secondary-button" type="button" onClick={onImportSound}>
          Import sound
        </button>
        <button className="secondary-button" type="button" onClick={onRevealSounds}>
          Open sounds folder
        </button>
      </div>
      <label className="toggle-label wide">
        <input className="toggle-switch" type="checkbox" checked={settings.showNotification} onChange={(event) => onChange({ showNotification: event.target.checked })} />
        Show clip saved popup notification
      </label>
      <label className="wide">
        Popup position
        <select
          value={settings.notificationPosition || 'top-right'}
          onChange={(event) => onChange({ notificationPosition: event.target.value as any })}
        >
          <option value="top-right">Top Right</option>
          <option value="top-left">Top Left</option>
          <option value="bottom-right">Bottom Right</option>
          <option value="bottom-left">Bottom Left</option>
          <option value="top-center">Top Center</option>
        </select>
      </label>
      <label className="toggle-label wide">
        <input className="toggle-switch" type="checkbox" checked={settings.startOnLogin} onChange={(event) => onChange({ startOnLogin: event.target.checked })} />
        Start silently on login
      </label>
    </div>
  )}

        {activeCategory === 'video' && (
          <div className="settings-group">
      <label>
        Display
        <select value={settings.monitorId || "primary"} onChange={(event) => onChange({ monitorId: event.target.value })}>
          <option value="primary">Primary display</option>
          {displayDevices.map((display) => (
            <option key={display.id} value={display.id}>
              {display.name} ({display.width}x{display.height}){display.isPrimary ? " primary" : ""}{display.hdr ? " HDR" : ""}
            </option>
          ))}
        </select>
      </label>
      <label>
        Clip length
        <input
          type="text"
          inputMode="numeric"
          pattern="[0-9]*"
          value={clipLengthDraft}
          onFocus={() => setEditingClipLength(true)}
          onChange={(event) => {
            const nextValue = event.target.value;
            if (/^\d*$/.test(nextValue)) setClipLengthDraft(nextValue);
          }}
          onBlur={() => {
            setEditingClipLength(false);
            commitClipLengthDraft();
          }}
          onKeyDown={(event) => {
            if (event.key === "Enter") {
              event.currentTarget.blur();
            } else if (event.key === "Escape") {
              setClipLengthDraft(String(settings.clipLengthSeconds));
              event.currentTarget.blur();
            }
          }}
        />
      </label>
      <label>
        FPS
        <select value={settings.fps} onChange={(event) => onChange({ fps: Number(event.target.value) as ClipSettings["fps"] })}>
          <option value={24}>24 low resource</option>
          <option value={30}>30 default</option>
          <option value={60}>60 high motion</option>
        </select>
      </label>
      <label>
        Resolution
        <select value={settings.resolutionPreset} onChange={(event) => onChange({ resolutionPreset: event.target.value as ClipSettings["resolutionPreset"] })}>
          <option value="system">System resolution</option>
          <option value="144p">144p</option>
          <option value="360p">360p</option>
          <option value="720p">720p</option>
          <option value="1080p">1080p</option>
          <option value="1440p">1440p</option>
          <option value="4k">4K</option>
        </select>
      </label>
      <label>
        Bitrate Mbps
        <DraftNumberInput
          min={4}
          max={120}
          value={settings.bitrateMbps}
          disabled={settings.autoBitrate}
          onCommit={(bitrateMbps) => onChange({ bitrateMbps })}
        />
      </label>
      <label className="toggle-label">
        <input
          className="toggle-switch"
          type="checkbox"
          checked={settings.autoBitrate}
          onChange={(event) => onChange({ autoBitrate: event.target.checked })}
        />
        Auto bitrate
      </label>
      <label>
        Max auto bitrate Mbps
        <DraftNumberInput
          min={4}
          max={120}
          value={settings.maxAutoBitrateMbps}
          disabled={!settings.autoBitrate}
          onCommit={(maxAutoBitrateMbps) => onChange({ maxAutoBitrateMbps })}
        />
      </label>
      <label>
        NVENC preset
        <select value={settings.nvencPreset} onChange={(event) => onChange({ nvencPreset: Number(event.target.value) as ClipSettings["nvencPreset"] })}>
          <option value={1}>P1 fastest</option>
          <option value={2}>P2 low resource</option>
          <option value={3}>P3 balanced</option>
          <option value={4}>P4 quality</option>
          <option value={5}>P5 higher quality</option>
        </select>
      </label>
    </div>
  )}

        {activeCategory === 'audio' && (
          <div className="settings-group single-column">
            <div className="audio-settings-panel">
              <div className="audio-settings-header">
                <div className="audio-settings-heading">
                  <h2>Audio sources</h2>
                  <p>Choose what audio to record and how it's captured.</p>
                </div>
                <button className="add-source wide-add-source" title="Add audio source" type="button" onClick={addSource}>
                  <Plus size={16} />
                  <span>Add source</span>
                </button>
              </div>

              <div className="audio-source-card">
                {builtInAudioSources.map((source) => (
                  <Fragment key={source.id}>
                    <div className="audio-source-row">
                      <div className="audio-source-main">
                        <span className="audio-source-icon" aria-hidden="true">
                          {audioSourceIcon(source)}
                        </span>
                        <span className="audio-source-text">
                          <strong>{source.label}</strong>
                          <span>{audioSourceDescription(source)}</span>
                        </span>
                      </div>
                      <div className="audio-source-actions">
                        <label className="audio-source-toggle" title={source.enabled ? "Disable this source" : "Enable this source"}>
                          <input
                            className="toggle-switch"
                            type="checkbox"
                            checked={source.enabled}
                            onChange={(event) => setAudioSourceEnabled(source.id, event.target.checked)}
                          />
                        </label>
                        {source.kind === "system" && (
                          <>
                            <button
                              className="secondary-button compact-configure"
                              type="button"
                              onClick={() => {
                                void window.clipture.listActiveProcesses().then(setActiveProcesses);
                                setConfiguringSystemAudio(true);
                              }}
                            >
                              Configure
                            </button>
                          </>
                        )}
                        {source.kind === "microphone" && (
                          <>
                            <button
                              className="secondary-button compact-configure"
                              type="button"
                              onClick={() => {
                                void window.clipture.listAudioInputDevices().then(setInputDevices);
                                setConfiguringMicAudio(source.id);
                              }}
                            >
                              Configure
                            </button>
                          </>
                        )}
                        {source.kind === "game" && (
                          <span className="audio-status-badge">Auto-detected</span>
                        )}
                      </div>
                    </div>
                    {source.kind === "system" && configuringSystemAudio && (
                      <SystemAudioModal
                        source={source}
                        activeProcesses={activeProcesses}
                        otherAppProcesses={otherAppProcesses}
                        onSave={(patch) => updateAudioSource(source.id, patch)}
                        onClose={() => setConfiguringSystemAudio(false)}
                      />
                    )}
                    {source.kind === "microphone" && configuringMicAudio === source.id && (
                      <MicrophoneSettingsModal
                        source={source}
                        inputDevices={inputDevices}
                        onUpdate={(patch) => updateAudioSource(source.id, patch)}
                        onClose={() => setConfiguringMicAudio(null)}
                      />
                    )}
                  </Fragment>
                ))}
              </div>

              <section className="separate-app-section">
                <div className="audio-section-copy">
                  <h3>Separate app tracks</h3>
                  <p>Apps you allow will be recorded on their own separate tracks. You can remove them anytime.</p>
                </div>
                <div className="audio-source-card app-track-card">
                  {appAudioSources.length === 0 ? (
                    <div className="audio-source-empty">No separate app tracks yet.</div>
                  ) : (
                    appAudioSources.map((source) => (
                      <Fragment key={source.id}>
                        <div
                          className="audio-source-row app-track-row"
                          onAuxClick={(event) => {
                            if (event.button !== 1) return;
                            event.preventDefault();
                            removeSource(source.id);
                          }}
                        >
                          <button
                            className="audio-source-main audio-source-main-button"
                            type="button"
                            onClick={() => {
                              void window.clipture.listActiveProcesses().then(setActiveProcesses);
                              setConfiguringAppAudio(source.id);
                            }}
                          >
                            <AppAudioSourceIcon source={source} fallback={appSourceInitial(source)} />
                            <span className="audio-source-text">
                              <strong>{appSourceName(source)}</strong>
                              <span>Separate track</span>
                            </span>
                          </button>
                          <div className="audio-source-actions">
                            <label className="audio-source-toggle" title={source.enabled ? "Disable this track" : "Enable this track"}>
                              <input
                                className="toggle-switch"
                                type="checkbox"
                                checked={source.enabled}
                                onChange={(event) => setAudioSourceEnabled(source.id, event.target.checked)}
                              />
                            </label>
                            <button
                              aria-label={`Delete ${appSourceName(source)}`}
                              className="audio-icon-button danger"
                              onClick={() => removeSource(source.id)}
                              title="Delete audio source"
                              type="button"
                            >
                              <Trash2 size={17} />
                            </button>
                          </div>
                        </div>
                        {configuringAppAudio === source.id && (
                          <AppAudioModal
                            source={source}
                            activeProcesses={activeProcesses}
                            onSave={(patch) => {
                              updateAudioSource(source.id, patch);
                              setConfiguringAppAudio(null);
                            }}
                            onClose={() => setConfiguringAppAudio(null)}
                          />
                        )}
                      </Fragment>
                    ))
                  )}
                </div>
              </section>
            </div>
          </div>
        )}

        {activeCategory === 'customize' && <CustomizeSettings settings={settings} onChange={onChange} />}
      </div>
    </section>
  );
}

function DiagnosticsView({ diagnostics }: { diagnostics: EngineDiagnostics }) {
  const replayMiB = (bytes: number) => `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
  const entries = [
    ["Capture API", diagnostics.captureApi],
    ["Requested capture backend", diagnostics.requestedCaptureBackend],
    ["Active capture backend", diagnostics.activeCaptureBackend],
    ["Capture fallback", diagnostics.captureFallbackReason || "None"],
    ["Display refresh", `${diagnostics.displayRefreshHz.toFixed(3)} Hz (${diagnostics.displayRefreshNumerator}/${diagnostics.displayRefreshDenominator})`],
    ["Desktop present rate", `${diagnostics.desktopPresentFps.toFixed(2)} FPS`],
    ["Published fresh rate", `${diagnostics.publishedFreshFps.toFixed(2)} FPS`],
    ["Recent capture / encoder in / encoder out", `${diagnostics.recentPublishedFreshFps.toFixed(2)} / ${diagnostics.recentEncoderInputFps.toFixed(2)} / ${diagnostics.recentEncoderOutputFps.toFixed(2)} FPS`],
    ["Encoded repeat ratio", `${(diagnostics.encodedRepeatRatio * 100).toFixed(2)}%`],
    ["Capture updates acquired", String(diagnostics.captureAcquiredUpdates)],
    ["Desktop presents", String(diagnostics.captureDesktopPresents)],
    ["Pointer updates", String(diagnostics.capturePointerUpdates)],
    ["Fresh frames published", String(diagnostics.capturePublishedFrames)],
    ["Accumulated source frames", String(diagnostics.captureAccumulatedFrames)],
    ["Accumulation events", String(diagnostics.captureAccumulationEvents)],
    ["Sampler rejections", String(diagnostics.captureSamplerRejections)],
    ["Non-monotonic timestamps", String(diagnostics.captureNonMonotonicTimestamps)],
    ["Acquire timeouts", String(diagnostics.captureAcquireTimeouts)],
    ["DXGI access losses", String(diagnostics.captureAccessLosses)],
    ["DXGI recreation attempts", String(diagnostics.captureRecreationAttempts)],
    ["DXGI recreation successes", String(diagnostics.captureRecreationSuccesses)],
    ["Capture fallbacks", String(diagnostics.captureFallbacks)],
    ["Encoder", diagnostics.activeEncoder],
    ["Encoder mode", diagnostics.encoderMode],
    ["GPU", diagnostics.gpu],
    ["Display", diagnostics.display],
    ["HDR tonemapping", diagnostics.hdrTonemapping ? "Enabled" : "Disabled"],
    ["Video source", diagnostics.videoSourceResolution],
    ["Output canvas", diagnostics.videoOutputResolution],
    ["Scaling", diagnostics.videoScaling],
    ["Clip target", diagnostics.clipTargetResolution],
    ["Microphone", diagnostics.microphoneDevice],
    ["Codec", diagnostics.codec],
    ["Resolution", diagnostics.resolution],
    ["FPS", String(diagnostics.fps)],
    ["Bitrate", `${diagnostics.bitrateMbps} Mbps`],
    ["Hardware acceleration", diagnostics.hardwareAcceleration ? "Enabled" : "Disabled"],
    ["Engine running", diagnostics.engineRunning ? "Yes" : "No"],
    ["D3D11 ready", diagnostics.d3d11Ready ? "Yes" : "No"],
    ["Capture ready", diagnostics.captureReady ? "Yes" : "No"],
    ["Audio ready", diagnostics.audioReady ? "Yes" : "No"],
    ["Mux ready", diagnostics.muxReady ? "Yes" : "No"],
    ["Buffer window", `${diagnostics.bufferDurationSeconds}s`],
    ["Captured frames", String(diagnostics.capturedFrames)],
    ["Queued frames", String(diagnostics.queuedFrames)],
    ["Encoder accepted", String(diagnostics.encoderAcceptedFrames)],
    ["Encoder packets", String(diagnostics.encoderOutputPackets)],
    ["Audio captured", String(diagnostics.audioCapturedPackets)],
    ["Video packets", String(diagnostics.bufferedVideoPackets)],
    ["Audio packets", String(diagnostics.bufferedAudioPackets)],
    ["Replay archive video / audio", `${diagnostics.videoReplayArchiveHealthy ? "Healthy" : "RAM fallback"} / ${diagnostics.audioReplayArchiveHealthy ? "Healthy" : "RAM fallback"}`],
    ["Replay archive disk", replayMiB(diagnostics.replayArchiveDiskBytes)],
    ["Replay RAM cache", `${replayMiB(diagnostics.replayArchiveResidentBytes)} / ${replayMiB(diagnostics.replayArchiveResidentBudgetBytes)}`],
    ["Replay read cache", replayMiB(diagnostics.replayArchiveReadCacheBytes)],
    ["Replay packets RAM / disk", `${diagnostics.replayArchiveResidentPackets} / ${diagnostics.replayArchiveDiskBackedPackets}`],
    ["Replay archive RAM fallback", replayMiB(diagnostics.replayArchiveRamFallbackBytes)],
    ["Replay archive queued", `${diagnostics.replayArchiveQueuedPackets} packets / ${replayMiB(diagnostics.replayArchiveQueuedBytes)}`],
    ["Replay archive segments", String(diagnostics.replayArchiveSegments)],
    ["Replay archive persisted", String(diagnostics.replayArchivePersistedPackets)],
    ["Replay spill inspections", String(diagnostics.replayArchiveSpillCandidateInspections)],
    ["PCM recovery", diagnostics.pcmRecoveryActive ? "Active" : "Standby"],
    ["Replay archive write failures", String(diagnostics.replayArchiveWriteFailures)],
    ["Replay archive max write", replayMiB(diagnostics.replayArchiveMaximumWriteBytes)],
    ["Dropped frames", String(diagnostics.droppedFrames)],
    ["Capture overflow", String(diagnostics.captureOverflowDrops)],
    ["Source frames superseded", String(diagnostics.sourceFramesSuperseded)],
    ["Capture queue coalesced", String(diagnostics.captureCoalescedDrops)],
    ["Owned-slot drops", String(diagnostics.captureSlotDrops)],
    ["Capture callback errors", String(diagnostics.captureCallbackErrors)],
    ["Scheduler skips", String(diagnostics.schedulerDroppedFrames)],
    ["Scheduler repeats", String(diagnostics.schedulerRepeatedFrames)],
    ["Encoder queue drops", String(diagnostics.encoderQueueDrops)],
    ["Encoder repeats coalesced", String(diagnostics.encoderRepeatCoalesced)],
    ["Encoder queued fresh / repeats", `${diagnostics.encoderQueuedFreshFrames} / ${diagnostics.encoderQueuedRepeatFrames}`],
    ["NVENC surface drops", String(diagnostics.nvencSurfaceDrops)],
    ["NVENC input drops", String(diagnostics.nvencInputDrops)],
    ["Encoder backpressure", String(diagnostics.encoderBackpressureDrops)],
    ["NVENC in flight", String(diagnostics.nvencInFlightFrames)],
    ["Maximum capture gap", `${(diagnostics.maximumCaptureGap100ns / 10_000).toFixed(1)} ms`],
    ["Maximum submit latency", `${(diagnostics.maximumSubmitLatency100ns / 10_000).toFixed(1)} ms`],
    ["Scale latency avg / max", `${(diagnostics.averageScaleLatency100ns / 10_000).toFixed(2)} / ${(diagnostics.maximumScaleLatency100ns / 10_000).toFixed(2)} ms`],
    ["Input map latency avg / max", `${(diagnostics.averageInputMapLatency100ns / 10_000).toFixed(2)} / ${(diagnostics.maximumInputMapLatency100ns / 10_000).toFixed(2)} ms`],
    ["NVENC call latency avg / max", `${(diagnostics.averageNvencCallLatency100ns / 10_000).toFixed(2)} / ${(diagnostics.maximumNvencCallLatency100ns / 10_000).toFixed(2)} ms`],
    ["Output drain latency avg / max", `${(diagnostics.averageOutputDrainLatency100ns / 10_000).toFixed(2)} / ${(diagnostics.maximumOutputDrainLatency100ns / 10_000).toFixed(2)} ms`],
    ["Capture acquire recent p95", `${(diagnostics.recentCaptureAcquireP95_100ns / 10_000).toFixed(2)} ms`],
    ["Capture preparation recent p50 / p95", `${(diagnostics.recentCapturePreparationP50_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentCapturePreparationP95_100ns / 10_000).toFixed(2)} ms`],
    ["Cursor composite recent p95", `${(diagnostics.recentCaptureCursorP95_100ns / 10_000).toFixed(2)} ms`],
    ["Capture processing recent p50 / p95", `${(diagnostics.recentCaptureProcessingP50_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentCaptureProcessingP95_100ns / 10_000).toFixed(2)} ms`],
    ["Input preparation recent p50 / p95", `${(diagnostics.recentInputPreparationP50_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentInputPreparationP95_100ns / 10_000).toFixed(2)} ms`],
    ["Input map recent p50 / p95", `${(diagnostics.recentInputMapP50_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentInputMapP95_100ns / 10_000).toFixed(2)} ms`],
    ["NVENC call recent p50 / p95", `${(diagnostics.recentNvencCallP50_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentNvencCallP95_100ns / 10_000).toFixed(2)} ms`],
    ["Output event wait recent p50 / p95", `${(diagnostics.recentOutputEventWaitP50_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentOutputEventWaitP95_100ns / 10_000).toFixed(2)} ms`],
    ["Output lock / copy / unmap recent p95", `${(diagnostics.recentOutputLockP95_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentOutputCopyP95_100ns / 10_000).toFixed(2)} / ${(diagnostics.recentOutputUnmapP95_100ns / 10_000).toFixed(2)} ms`],
    ["NVENC input paths zero-copy / copied / converted", `${diagnostics.nvencZeroCopyFrames} / ${diagnostics.nvencCopyFallbackFrames} / ${diagnostics.nvencConvertedFrames}`],
    ["Capture epoch", String(diagnostics.captureEpoch)],
    ["Capture pressure", diagnostics.capturePressure]
  ];

  return (
    <section className="diagnostics panel">
      {entries.map(([label, value]) => (
        <div className="metric" key={label}>
          <span>{label}</span>
          <strong>{value}</strong>
        </div>
      ))}
    </section>
  );
}
