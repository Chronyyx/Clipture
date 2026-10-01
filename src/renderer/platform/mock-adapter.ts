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
    clipPlaybackUrl: async (filePath) => ({ url: playable(filePath), mixed: false, message: 'Mock playback' }),
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
