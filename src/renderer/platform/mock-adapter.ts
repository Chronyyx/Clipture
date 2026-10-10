import type {
  ActiveProcess,
  AudioInputDevice,
  ClipRecord,
  ClipSettings,
  ClipSoundOption,
  CliptureApi,
  DisplayDevice,
  EngineDiagnostics,
  SaveIoAnalyzerState,
  UpdateState
} from '../../shared/types';
import { defaultDiagnostics } from '../shared/diagnostics/defaultDiagnostics';
import { defaultSettings } from './defaultSettings';
import { mergeDiagnostics } from './diagnostics';
import { HostCapabilityError } from './hostError';

type Listener<T> = (payload: T) => void;

class MockEvent<T> {
  private listeners = new Set<Listener<T>>();

  subscribe(listener: Listener<T>) {
    this.listeners.add(listener);
    let disposed = false;
    return () => {
      if (disposed) return;
      disposed = true;
      this.listeners.delete(listener);
    };
  }

  emit(payload: T) {
    for (const listener of [...this.listeners]) listener(payload);
  }
}

export interface MockCliptureState {
  diagnostics?: Partial<EngineDiagnostics>;
  settings?: ClipSettings;
  clips?: ClipRecord[];
  sounds?: ClipSoundOption[];
  processes?: ActiveProcess[];
  audioInputs?: AudioInputDevice[];
  displays?: DisplayDevice[];
  update?: UpdateState;
  /** Browser preview only: simulated host latency and clip thumbnails. */
  latencyMs?: number;
  thumbnails?: Record<string, string>;
  /** Public web demo: one playable sample clip, working saves, and
   * plain-language messages for features that need the desktop app. */
  demo?: { playbackUrl: string };
}

export interface MockCliptureController {
  client: CliptureApi;
  emitLibraryChanged(clip?: ClipRecord): void;
  emitUpdateStateChanged(state: UpdateState): void;
  emitPlaySound(sound: string): void;
  emitShowNotification(thumbnailUrl: string, position: string, message?: string): void;
}

function delay(ms = 0): Promise<void> {
  return ms > 0 ? new Promise((resolve) => setTimeout(resolve, ms)) : Promise.resolve();
}

function clone<T>(value: T): T {
  return structuredClone(value);
}

/** A made-up but steady waveform for the preview, in the host's format
 * (audiowaveform JSON): talk and game noise with a few loud moments, the
 * same for the same clip every time. */
function sampleWaveform(seed: string, seconds: number) {
  let state = [...seed].reduce((hash, character) => (hash * 31 + character.charCodeAt(0)) >>> 0, 7);
  const next = () => ((state = (state * 1664525 + 1013904223) >>> 0) / 2 ** 32);
  const length = Math.max(1, Math.round(seconds * 100));
  const data: number[] = [];
  let burst = 0;
  for (let pair = 0; pair < length; pair += 1) {
    if (next() > 0.997) burst = 1;
    burst *= 0.93;
    const talk = Math.max(0, Math.sin(pair / 23) * Math.sin(pair / 151)) * 0.35;
    const level = Math.min(1, 0.04 + talk + burst * 0.9 + next() * 0.05);
    data.push(-Math.round(127 * level * (0.6 + next() * 0.4)), Math.round(127 * level * (0.6 + next() * 0.4)));
  }
  const waveform = { version: 2, channels: 1, sample_rate: 8000, samples_per_pixel: 80, bits: 8, length, data };
  return 'data:application/json,' + encodeURIComponent(JSON.stringify(waveform));
}

export function createMockCliptureController(seed: MockCliptureState = {}): MockCliptureController {
  const unavailable = (capability: string): never => {
    if (seed.demo) throw new Error("This needs the Clipture desktop app. The web demo can't reach your PC.");
    throw new HostCapabilityError('mock', capability, 'This operation requires a native desktop host');
  };
  const playable = (filePath: string) => seed.demo?.playbackUrl ?? filePath;
  let demoSaves = 0;

  let settings = clone(seed.settings ?? defaultSettings);
  let clips = clone(seed.clips ?? []);
  let update: UpdateState = clone(seed.update ?? { status: 'idle' });
  let saveIo: SaveIoAnalyzerState = { available: false, armed: false, traceReady: false };
  const libraryChanged = new MockEvent<ClipRecord | undefined>();
  const updateChanged = new MockEvent<UpdateState>();
  const playSound = new MockEvent<string>();
  const showNotification = new MockEvent<{ thumbnailUrl: string; position: string; message?: string }>();

  const client: CliptureApi = {
    getDiagnostics: async () => mergeDiagnostics(seed.diagnostics ?? defaultDiagnostics),
    exportDiagnostics: async () => unavailable('exportDiagnostics'),
    getSaveIoAnalyzerState: async () => clone(saveIo),
    setSaveIoAnalyzerArmed: async (armed) => clone(saveIo = { ...saveIo, armed }),
    getSettings: async () => clone(settings),
    saveSettings: async (next) => clone(settings = clone(next)),
    saveClip: async () => {
      if (!seed.demo) return unavailable('saveClip');
      // A save in the demo copies the newest clip's look as a fresh clip.
      const source = clips[0];
      const now = new Date();
      demoSaves += 1;
      const filePath = 'C:\\Users\\you\\Videos\\Clipture\\Demo\\saved-' + demoSaves + '.mp4';
      const clip: ClipRecord = {
        id: 'demo-save-' + demoSaves,
        title: 'Clipture clip',
        gameOrApp: source?.gameOrApp ?? 'Desktop',
        librarySource: 'clip',
        isGame: source?.isGame ?? false,
        createdAt: now.toISOString(),
        durationSeconds: settings.clipLengthSeconds,
        filePath,
        resolution: source?.resolution ?? '1920x1080',
        fps: settings.fps,
        encoder: source?.encoder ?? 'NVENC H.264',
        audioTracks: source?.audioTracks ?? []
      };
      if (source && seed.thumbnails?.[source.filePath]) seed.thumbnails[filePath] = seed.thumbnails[source.filePath];
      clips = [clip, ...clips];
      libraryChanged.emit(clone(clip));
      return { ok: true, message: 'Saved the last ' + settings.clipLengthSeconds + 's.', clip: clone(clip) };
    },
    listClips: async () => { await delay(seed.latencyMs); return clone(clips); },
    deleteClips: async (ids) => {
      clips = clips.filter((clip) => !ids.includes(clip.id));
      libraryChanged.emit(undefined);
      return true;
    },
    importVideoFolders: async () => unavailable('importVideoFolders'),
    clipUrl: async (filePath) => playable(filePath),
    clipIconUrl: async () => '',
    processIconUrl: async () => '',
    clipThumbnailUrl: async (filePath) => { await delay(seed.latencyMs); return seed.thumbnails?.[filePath] ?? ''; },
    clipPlaybackUrl: async (filePath, audioTracks) => ({
      url: playable(filePath),
      mixed: false,
      message: 'Mock playback',
      waveformUrl: audioTracks.length > 0
        ? sampleWaveform(filePath, clips.find((clip) => clip.filePath === filePath)?.durationSeconds ?? 60)
        : undefined
    }),
    releasePlaybackCache: async () => true,
    getClipRepairStatus: async () => ({ phase: 'idle', checked: 0, total: 0, needsRepair: 0, needsRepairBytes: 0, repaired: 0, failed: 0 }),
    checkClipLayouts: async () => unavailable('checkClipLayouts'),
    fixClipLayouts: async () => unavailable('fixClipLayouts'),
    listActiveProcesses: async () => clone(seed.processes ?? []),
    listAudioInputDevices: async () => { await delay(seed.latencyMs); return clone(seed.audioInputs ?? []); },
    listDisplayDevices: async () => { await delay(seed.latencyMs); return clone(seed.displays ?? []); },
    listClipSounds: async () => clone(seed.sounds ?? []),
    importClipSound: async () => unavailable('importClipSound'),
    revealSoundsFolder: async () => unavailable('revealSoundsFolder'),
    revealClip: async () => unavailable('revealClip'),
    renameClip: async (id, newTitle) => {
      const clip = clips.find((candidate) => candidate.id === id);
      if (!clip) return false;
      clip.title = newTitle;
      libraryChanged.emit(clone(clip));
      return true;
    },
    // The preview keeps the kept length; its sample files stay as they are.
    trimClip: async (id, startSeconds, endSeconds) => {
      const clip = clips.find((candidate) => candidate.id === id);
      if (!clip) throw new Error('That clip is no longer in the library.');
      await new Promise((resolve) => setTimeout(resolve, 900));
      const before = 40_000_000;
      clip.durationSeconds = Math.max(1, Math.round(endSeconds - startSeconds));
      libraryChanged.emit(clone(clip));
      return { durationSeconds: endSeconds - startSeconds, bytesBefore: before, bytesAfter: Math.round(before * (endSeconds - startSeconds) / 60) };
    },
    // The preview's sample files stay as they are.
    mixClipAudio: async (id, gains) => {
      if (!clips.some((candidate) => candidate.id === id)) throw new Error('That clip is no longer in the library.');
      // About as long as re-encoding a short clip's tracks takes.
      await new Promise((resolve) => setTimeout(resolve, 1400));
      const before = 40_000_000;
      return { tracksChanged: gains.filter((gain) => Math.abs(gain - 1) > 0.005).length, bytesBefore: before, bytesAfter: before + 12_000 };
    },
    getUpdateState: async () => clone(update),
    checkForUpdates: async () => clone(update),
    downloadUpdate: async () => unavailable('downloadUpdate'),
    installUpdate: async () => unavailable('installUpdate'),
    openThemeFontDownload: async () => unavailable('openThemeFontDownload'),
    onLibraryChanged: (callback) => libraryChanged.subscribe(callback),
    onUpdateStateChanged: (callback) => updateChanged.subscribe(callback),
    onPlaySound: (callback) => playSound.subscribe(callback),
    onShowNotification: (callback) => showNotification.subscribe(({ thumbnailUrl, position, message }) => callback(thumbnailUrl, position, message)),
    hideNotification: () => undefined,
    selectFolder: async () => unavailable('selectFolder')
  };

  return {
    client,
    emitLibraryChanged: (clip) => libraryChanged.emit(clone(clip)),
    emitUpdateStateChanged: (state) => {
      update = clone(state);
      updateChanged.emit(clone(state));
    },
    emitPlaySound: (sound) => playSound.emit(sound),
    emitShowNotification: (thumbnailUrl, position, message) => showNotification.emit({ thumbnailUrl, position, message })
  };
}

export function createMockCliptureAdapter(seed?: MockCliptureState): CliptureApi {
  return createMockCliptureController(seed).client;
}
