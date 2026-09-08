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
}

export interface MockCliptureController {
  client: CliptureApi;
  emitLibraryChanged(clip?: ClipRecord): void;
  emitUpdateStateChanged(state: UpdateState): void;
  emitPlaySound(sound: string): void;
  emitShowNotification(thumbnailUrl: string, position: string, message?: string): void;
}

function clone<T>(value: T): T {
  return structuredClone(value);
}

function unavailable(capability: string): never {
  throw new HostCapabilityError('mock', capability, 'This operation requires a native desktop host');
}

export function createMockCliptureController(seed: MockCliptureState = {}): MockCliptureController {
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
    saveClip: async () => unavailable('saveClip'),
    listClips: async () => clone(clips),
    deleteClips: async (ids) => {
      clips = clips.filter((clip) => !ids.includes(clip.id));
      libraryChanged.emit(undefined);
      return true;
    },
    importVideoFolders: async () => unavailable('importVideoFolders'),
    clipUrl: async (filePath) => filePath,
    clipIconUrl: async () => '',
    processIconUrl: async () => '',
    clipThumbnailUrl: async () => '',
    clipPlaybackUrl: async (filePath) => ({ url: filePath, mixed: false, message: 'Mock playback' }),
    releasePlaybackCache: async () => true,
    listActiveProcesses: async () => clone(seed.processes ?? []),
    listAudioInputDevices: async () => clone(seed.audioInputs ?? []),
    listDisplayDevices: async () => clone(seed.displays ?? []),
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
