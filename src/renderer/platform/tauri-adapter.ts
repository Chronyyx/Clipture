import { convertFileSrc, invoke } from '@tauri-apps/api/core';
import type {
  ActiveProcess,
  AudioInputDevice,
  ClipRecord,
  ClipSettings,
  ClipSoundOption,
  CliptureApi,
  DisplayDevice,
  EngineDiagnostics,
  SaveClipResult,
  SaveIoAnalyzerState,
  ThemeFontId,
  UpdateState
} from '../../shared/types';
import { diagnosticsUnavailable, mergeDiagnostics } from './diagnostics';
import { HostCapabilityError } from './hostError';
import { preserveSaveResult } from './save-result';
import { subscribeToTauriEvent } from './tauri-events';

const commands = {
  getDiagnostics: 'get_diagnostics',
  exportDiagnostics: 'export_diagnostics',
  getSaveIoAnalyzerState: 'get_save_io_analyzer_state',
  setSaveIoAnalyzerArmed: 'set_save_io_analyzer_armed',
  getSettings: 'get_settings',
  saveSettings: 'save_settings',
  saveClip: 'save_clip',
  listClips: 'list_clips',
  deleteClips: 'delete_clips',
  importVideoFolders: 'import_video_folders',
  clipUrl: 'clip_url',
  clipIconUrl: 'clip_icon_url',
  processIconUrl: 'process_icon_url',
  clipThumbnailUrl: 'clip_thumbnail_url',
  clipPlaybackUrl: 'clip_playback_url',
  releasePlaybackCache: 'release_playback_cache',
  listActiveProcesses: 'list_active_processes',
  listAudioInputDevices: 'list_audio_input_devices',
  listDisplayDevices: 'list_display_devices',
  listClipSounds: 'list_clip_sounds',
  importClipSound: 'import_clip_sound',
  revealSoundsFolder: 'reveal_sounds_folder',
  revealClip: 'reveal_clip',
  renameClip: 'rename_clip',
  getUpdateState: 'get_update_state',
  checkForUpdates: 'check_for_updates',
  downloadUpdate: 'download_update',
  installUpdate: 'install_update',
  openThemeFontDownload: 'open_theme_font_download',
  hideNotification: 'hide_notification',
  selectFolder: 'select_folder'
} as const;

type CommandName = keyof typeof commands;

function call<T>(capability: CommandName, args?: Record<string, unknown>): Promise<T> {
  const command = commands[capability];
  return invoke<T>(command, args).catch((error) => {
    throw new HostCapabilityError('tauri', capability, error, command);
  });
}

function isUrl(value: string) {
  return /^(?:asset|blob|clipture-media|data|file|https?|tauri):/i.test(value);
}

function resourceUrl(value: string): string {
  return !value || isUrl(value) ? value : convertFileSrc(value);
}

function soundUrls(sounds: ClipSoundOption[]) {
  return sounds.map((sound) => sound.url ? { ...sound, url: resourceUrl(sound.url) } : sound);
}

export function hasTauriRuntime(): boolean {
  return '__TAURI_INTERNALS__' in window;
}

export function createTauriAdapter(): CliptureApi {
  return {
    getDiagnostics: async () => {
      try {
        return mergeDiagnostics(await call<Partial<EngineDiagnostics>>('getDiagnostics'));
      } catch (error) {
        return diagnosticsUnavailable(error);
      }
    },
    exportDiagnostics: () => call<string | undefined>('exportDiagnostics'),
    getSaveIoAnalyzerState: () => call<SaveIoAnalyzerState>('getSaveIoAnalyzerState'),
    setSaveIoAnalyzerArmed: (armed) => call<SaveIoAnalyzerState>('setSaveIoAnalyzerArmed', { armed }),
    getSettings: () => call<ClipSettings>('getSettings'),
    saveSettings: (settings) => call<ClipSettings>('saveSettings', { settings }),
    saveClip: async (durationSeconds) => preserveSaveResult(await call<SaveClipResult>('saveClip', { durationSeconds })),
    listClips: () => call<ClipRecord[]>('listClips'),
    deleteClips: (ids) => call<boolean>('deleteClips', { ids }),
    importVideoFolders: () => call<boolean>('importVideoFolders'),
    clipUrl: async (filePath) => resourceUrl(await call<string>('clipUrl', { filePath })),
    clipIconUrl: async (clip, preferredLabels) => resourceUrl(await call<string>('clipIconUrl', { clip, preferredLabels })),
    processIconUrl: async (processName, executablePath) => resourceUrl(await call<string>('processIconUrl', { processName, executablePath })),
    clipThumbnailUrl: async (filePath) => resourceUrl(await call<string>('clipThumbnailUrl', { filePath })),
    clipPlaybackUrl: async (filePath, audioTracks) => {
      const result = await call<Awaited<ReturnType<CliptureApi['clipPlaybackUrl']>>>('clipPlaybackUrl', { filePath, audioTracks });
      return {
        ...result,
        url: resourceUrl(result.url),
        audioChunkUrl: result.audioChunkUrl ? resourceUrl(result.audioChunkUrl) : undefined
      };
    },
    releasePlaybackCache: () => call<boolean>('releasePlaybackCache'),
    listActiveProcesses: () => call<ActiveProcess[]>('listActiveProcesses'),
    listAudioInputDevices: () => call<AudioInputDevice[]>('listAudioInputDevices'),
    listDisplayDevices: () => call<DisplayDevice[]>('listDisplayDevices'),
    listClipSounds: async () => soundUrls(await call<ClipSoundOption[]>('listClipSounds')),
    importClipSound: async () => {
      const sound = await call<ClipSoundOption | undefined>('importClipSound');
      return sound?.url ? { ...sound, url: resourceUrl(sound.url) } : sound;
    },
    revealSoundsFolder: () => call<void>('revealSoundsFolder'),
    revealClip: (filePath) => call<void>('revealClip', { filePath }),
    renameClip: (id, newTitle) => call<boolean>('renameClip', { id, newTitle }),
    getUpdateState: () => call<UpdateState>('getUpdateState'),
    checkForUpdates: () => call<UpdateState>('checkForUpdates'),
    downloadUpdate: () => call<void>('downloadUpdate'),
    installUpdate: () => call<void>('installUpdate'),
    openThemeFontDownload: (theme: ThemeFontId) => call<void>('openThemeFontDownload', { theme }),
    onLibraryChanged: (callback) => subscribeToTauriEvent<ClipRecord | null>('library://changed', (clip) => callback(clip ?? undefined)),
    onUpdateStateChanged: (callback) => subscribeToTauriEvent<UpdateState>('updates://state-changed', callback),
    onPlaySound: (callback) => subscribeToTauriEvent<string>('sounds://play', callback),
    onShowNotification: (callback) => subscribeToTauriEvent<{ thumbnailUrl: string; position: string; message?: string }>(
      'notifications://show',
      ({ thumbnailUrl, position, message }) => callback(resourceUrl(thumbnailUrl), position, message)
    ),
    hideNotification: () => {
      void call<void>('hideNotification').catch((error) => console.error(error));
    },
    selectFolder: (currentPath) => call<string | undefined>('selectFolder', { currentPath })
  };
}
