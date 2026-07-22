import { Activity, Check, ChevronDown, Clapperboard, Clock, Download, Edit3, FolderOpen, Gamepad2, Library, Maximize2, Mic, Pause, Play, Plus, RefreshCw, Save, Search, SlidersHorizontal, Trash2, Upload, Volume2, X } from "lucide-react";
import { Fragment, useEffect, useMemo, useRef, useState } from "react";
// @ts-ignore
import logoUrl from "../../assets/svgviewer-output.svg";
import type { KeyboardEvent } from "react";
import type { ActiveProcess, AudioInputDevice, ClipRecord, ClipSettings, DisplayDevice, EngineDiagnostics, AudioSourceRule, ClipSoundOption, UpdateState } from "../shared/types";

type Tab = "library" | "settings" | "diagnostics";

const defaultDiagnostics: EngineDiagnostics = {
  captureApi: "Windows.Graphics.Capture",
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
  const [notice, setNotice] = useState("");
  const [selectedClip, setSelectedClip] = useState<ClipRecord | undefined>();
  const [clipSounds, setClipSounds] = useState<ClipSoundOption[]>([]);
  const [updateState, setUpdateState] = useState<UpdateState>(defaultUpdateState);
  const [isSavingClip, setIsSavingClip] = useState(false);
  const clipSoundUrlsRef = useRef<Record<string, string>>({});

  async function refresh() {
    const [nextDiagnostics, nextSettings, nextClips, nextClipSounds] = await Promise.all([
      window.clipture.getDiagnostics(),
      window.clipture.getSettings(),
      window.clipture.listClips(),
      window.clipture.listClipSounds()
    ]);
    setDiagnostics(nextDiagnostics);
    setSettings(nextSettings);
    setClips(nextClips);
    setClipSounds(nextClipSounds);
    clipSoundUrlsRef.current = Object.fromEntries(nextClipSounds.filter((sound) => sound.url).map((sound) => [sound.id, sound.url as string]));
  }

  useEffect(() => {
    void refresh().catch((error) => {
      console.error("Failed to refresh app state:", error);
      setNotice(error instanceof Error ? error.message : "Could not refresh app state.");
    });
    const timer = window.setInterval(() => {
      void window.clipture.getDiagnostics().then(setDiagnostics).catch((error) => {
        console.warn("Failed to refresh diagnostics:", error);
      });
    }, 2000);
    const unsubscribeLibrary = window.clipture.onLibraryChanged(() => {
      void refresh().catch((error) => console.error("Failed to refresh library:", error));
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

  async function saveClip() {
    if (isSavingClip) return;
    const length = settings?.clipLengthSeconds ?? 30;
    setIsSavingClip(true);
    try {
      const result = await window.clipture.saveClip(length);
      setNotice(result.message);
      await refresh();
    } catch (error) {
      setNotice(error instanceof Error ? error.message : "Could not save clip.");
    } finally {
      setIsSavingClip(false);
    }
  }

  async function importVideos() {
    const imported = await window.clipture.importVideoFolders();
    if (imported) {
      setNotice("Imported video folder added");
      await refresh();
    }
  }

  async function updateSettings(patch: Partial<ClipSettings>) {
    if (!settings) return;
    const saved = await window.clipture.saveSettings({ ...settings, ...patch });
    setSettings(saved);
    setNotice("Settings saved");
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
      <div className="titlebar-drag-region" />
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
            <button className="primary" onClick={saveClip} disabled={isSavingClip}>
              <Save size={18} /> {isSavingClip ? "Saving..." : `Save last ${settings?.clipLengthSeconds ?? 30}s`}
            </button>
          </header>
        )}

        {notice && <div className="notice">{notice}</div>}
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

  const selectedCount = selectedClipIds.size;

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
              <Clapperboard size={28} />
              <h1>Clip Library</h1>
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
                <button className="secondary-button select-clips-button" type="button" onClick={() => setSelectionMode(true)}>
                  <span className="select-clips-empty-box" aria-hidden="true" />
                  Select clips
                </button>
              </div>
            )}
          </div>
        </div>

        {selectedClip && <ClipPlayer clip={selectedClip} onClose={() => setSelectedClip(undefined)} settings={settings} />}

        {filteredClips.length === 0 ? (
          <LibraryEmptyState
            title={emptyTitle}
            copy={emptyCopy}
            detail={emptyDetail}
            actionLabel={libraryTab === "clips" ? "Save your first clip" : "Import videos"}
            onAction={libraryTab === "clips" ? onSaveClip : handleImportVideos}
          />
        ) : (
          <div className="clip-grid">
            {filteredClips.map((clip) => (
              <ClipCard
                clip={clip}
                isActive={selectedClip?.id === clip.id}
                isSelected={selectedClipIds.has(clip.id)}
                key={clip.id}
                onPlay={() => setSelectedClip(clip)}
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

function useClipIconUrl(clip: ClipRecord, preferredLabels: string[]): string {
  const [iconUrl, setIconUrl] = useState("");
  const focusedAppsKey = (clip.focusedApps ?? []).join("|");
  const audioTracksKey = clip.audioTracks.join("|");
  const preferredLabelsKey = preferredLabels.join("|");

  useEffect(() => {
    let active = true;
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
  }, [clip.id, clip.gameOrApp, focusedAppsKey, audioTracksKey, preferredLabelsKey]);

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
  const [isEditingTitle, setIsEditingTitle] = useState(false);
  const [editTitle, setEditTitle] = useState(displayTitle);
  const sourceLabels = useMemo(() => clipSourceLabels(clip, settings), [clip, settings]);
  const iconUrl = useClipIconUrl(clip, sourceLabels);

  useEffect(() => {
    let active = true;
    window.clipture.clipThumbnailUrl(clip.filePath).then((url) => {
      if (active && url) setThumbnailUrl(url);
    });
    return () => {
      active = false;
    };
  }, [clip.filePath]);

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
    <article className={[isActive ? "clip-card active" : "clip-card", isSelected ? "selected" : "", selectionMode ? "selectable" : ""].filter(Boolean).join(" ")}>
      <button className="thumbnail-button" onClick={selectionMode ? onToggleSelected : onPlay}>
        {selectionMode && (
          <span className={isSelected ? "clip-select-box checked" : "clip-select-box"}>
            {isSelected && <Check size={18} />}
          </span>
        )}
        {thumbnailUrl ? <img src={thumbnailUrl} alt="" /> : <div className="thumbnail-fallback">{clip.encoder}</div>}
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
  const minutes = Math.floor(safeSeconds / 60);
  const remainder = safeSeconds % 60;
  return minutes > 0 ? `${minutes}:${String(remainder).padStart(2, "0")}` : `0:${String(remainder).padStart(2, "0")}`;
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

function ClipPlayer({ clip, onClose, settings }: { clip: ClipRecord; onClose: () => void; settings?: ClipSettings }) {
  const videoRef = useRef<HTMLVideoElement | null>(null);
  const fastHoldStartedAtRef = useRef(0);
  const holdTimeoutRef = useRef<number | null>(null);
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
  const sourceLabels = useMemo(() => clipSourceLabels(clip, settings), [clip, settings]);
  const sourceText = sourceLabels.length > 0 ? sourceLabels.join(", ") : clip.gameOrApp;
  const iconUrl = useClipIconUrl(clip, sourceLabels);

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
  const mixedPlayRequestRef = useRef(0);

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
    if (!gain) return;

    const playbackRate = Math.max(0.1, Math.abs(video.playbackRate || 1));
    const videoTime = video.currentTime;
    const chunkEnd = chunk.start + chunk.buffer.duration;
    if (videoTime >= chunkEnd - 0.05) return;

    const offset = Math.max(0, Math.min(chunk.buffer.duration - 0.05, videoTime - chunk.start));
    const secondsUntilChunk = Math.max(0, (chunk.start - videoTime) / playbackRate);
    const source = ctx.createBufferSource();
    source.buffer = chunk.buffer;
    source.playbackRate.value = playbackRate;
    source.connect(gain);
    const scheduledNodes = [source];
    source.onended = () => {
      if (mixedScheduledChunksRef.current.get(chunkIndex) === scheduledNodes) {
        mixedScheduledChunksRef.current.delete(chunkIndex);
      }
    };

    mixedScheduledChunksRef.current.set(chunkIndex, scheduledNodes);
    chunk.lastUsedAt = performance.now();
    try {
      source.start(ctx.currentTime + secondsUntilChunk + 0.035, offset);
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

    for (let chunkIndex = firstChunk; chunkIndex <= lastChunk; chunkIndex += 1) {
      void loadMixedChunk(chunkIndex, generation).then(() => scheduleMixedChunk(chunkIndex, generation));
      scheduleMixedChunk(chunkIndex, generation);
    }

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
    if (!video) return;
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
    }

    const ctx = ensureAudioContext();
    try {
      await ctx.resume();
    } catch (error) {
      console.warn("Mixed audio resume failed:", error);
    }
    if (requestId !== mixedPlayRequestRef.current || generation !== mixedGenerationRef.current) {
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
    mixedTimerRef.current = window.setInterval(tick, 500);

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
    if (video.paused) {
      void playMixedWhenReady();
    } else {
      video.pause();
    }
  };

  const toggleFullscreen = () => {
    const shell = videoRef.current?.parentElement;
    if (document.fullscreenElement) {
      document.exitFullscreen().catch(console.error);
    } else {
      shell?.requestFullscreen().catch(console.error);
    }
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
      clearMixedAudio();
      if (audioCtxRef.current && audioCtxRef.current.state !== "closed") {
        audioCtxRef.current.close().catch(console.error);
      }
    };
  }, []);

  const beginFastHold = () => {
    fastHoldStartedAtRef.current = Date.now();
    if (holdTimeoutRef.current) window.clearTimeout(holdTimeoutRef.current);
    holdTimeoutRef.current = window.setTimeout(() => {
      const video = videoRef.current;
      if (!video) return;
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

  const progressPercent = duration > 0 ? Math.min(100, Math.max(0, (currentTime / duration) * 100)) : 0;

  const aspectWidth = parseInt((clip.resolution || "").split("x")[0]) || 16;
  const aspectHeight = parseInt((clip.resolution || "").split("x")[1]) || 9;

  return (
    <section className="player panel">
      <div className="player-header">
        <div>
          <strong className="player-title">
            {iconUrl && <img className="clip-app-icon" src={iconUrl} alt="" />}
            <span>{clip.title}</span>
          </strong>
          <span>
            {sourceText}
          </span>
        </div>
        <button className="icon-button" title="Close player" onClick={onClose}>
          <X size={16} />
        </button>
      </div>
      {src ? (
        <div
          className={`custom-video-shell ${controlsVisible ? "" : "controls-hidden"}`}
          style={{ maxWidth: `calc(480px * (${aspectWidth} / ${aspectHeight}))` }}
          onPointerMove={showControls}
          onPointerEnter={showControls}
          onPointerDown={(event) => {
            if ((event.target as HTMLElement).closest(".player-controls")) return;
            event.currentTarget.setPointerCapture(event.pointerId);
            beginFastHold();
          }}
          onPointerUp={(event) => {
            if (event.currentTarget.hasPointerCapture(event.pointerId)) {
              event.currentTarget.releasePointerCapture(event.pointerId);
            }
            endFastHold();
          }}
          onLostPointerCapture={endFastHold}
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
            onClick={() => {
              if (Date.now() - fastHoldStartedAtRef.current > 180) return;
              togglePlayback();
            }}
            onDoubleClick={toggleFullscreen}
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
              if (!mixedPlayback) return;
              if (event.currentTarget.paused) {
                ensureMixedBuffered();
              } else {
                void playMixedWhenReady();
              }
            }}
            onRateChange={(event) => {
              if (mixedPlayback && !event.currentTarget.paused) void restartMixedAudio();
            }}
            onEnded={() => {
              setPlaying(false);
              if (mixedPlayback) stopMixedAudio();
              endFastHold();
            }}
          />
          {holdingFast && <div className="speed-pill">2x</div>}
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
      <div className="player-meta">
        <span>{clip.durationSeconds}s</span>
        <span>{clip.resolution}</span>
        <span>{clip.fps} FPS</span>
        <span>{displayAudioTracks(clip.audioTracks) || "No audio tracks"}</span>
        <span>{message}</span>
      </div>
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
  const [activeCategory, setActiveCategory] = useState<"general" | "video" | "audio">("general");
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
        <input
          type="number"
          min={4}
          max={120}
          value={settings.bitrateMbps}
          disabled={settings.autoBitrate}
          onChange={(event) => onChange({ bitrateMbps: Number(event.target.value) })}
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
        <input
          type="number"
          min={4}
          max={120}
          value={settings.maxAutoBitrateMbps}
          disabled={!settings.autoBitrate}
          onChange={(event) => onChange({ maxAutoBitrateMbps: Number(event.target.value) })}
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
      </div>
    </section>
  );
}

function DiagnosticsView({ diagnostics }: { diagnostics: EngineDiagnostics }) {
  const entries = [
    ["Capture API", diagnostics.captureApi],
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
    ["Dropped frames", String(diagnostics.droppedFrames)]
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
