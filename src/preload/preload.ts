import { contextBridge, ipcRenderer } from "electron";
import type { ClipSettings, CliptureApi } from "../shared/types";

const api: CliptureApi = {
  getDiagnostics: () => ipcRenderer.invoke("engine:getDiagnostics"),
  getSettings: () => ipcRenderer.invoke("settings:get"),
  saveSettings: (settings: ClipSettings) => ipcRenderer.invoke("settings:save", settings),
  saveClip: (durationSeconds: number) => ipcRenderer.invoke("engine:saveClip", durationSeconds),
  listClips: () => ipcRenderer.invoke("library:list"),
  clipUrl: (filePath: string) => ipcRenderer.invoke("library:clipUrl", filePath),
  clipThumbnailUrl: (filePath: string) => ipcRenderer.invoke("library:clipThumbnailUrl", filePath),
  clipPlaybackUrl: (filePath: string, audioTracks: string[]) => ipcRenderer.invoke("library:clipPlaybackUrl", filePath, audioTracks),
  listActiveProcesses: () => ipcRenderer.invoke("processes:list"),
  listAudioInputDevices: () => ipcRenderer.invoke("audio:listInputDevices"),
  listDisplayDevices: () => ipcRenderer.invoke("displays:list"),
  listClipSounds: () => ipcRenderer.invoke("sounds:list"),
  importClipSound: () => ipcRenderer.invoke("sounds:import"),
  revealSoundsFolder: () => ipcRenderer.invoke("sounds:reveal"),
  revealClip: (filePath: string) => ipcRenderer.invoke("library:reveal", filePath),
  renameClip: (id: string, newTitle: string) => ipcRenderer.invoke("library:rename", id, newTitle),
  getUpdateState: () => ipcRenderer.invoke("updates:getState"),
  checkForUpdates: () => ipcRenderer.invoke("updates:check"),
  downloadUpdate: () => ipcRenderer.invoke("updates:download"),
  installUpdate: () => ipcRenderer.invoke("updates:install"),
  onLibraryChanged: (callback: () => void) => {
    const listener = () => callback();
    ipcRenderer.on("library:changed", listener);
    return () => ipcRenderer.removeListener("library:changed", listener);
  },
  onUpdateStateChanged: (callback) => {
    const listener = (_event: any, state: any) => callback(state);
    ipcRenderer.on("updates:stateChanged", listener);
    return () => ipcRenderer.removeListener("updates:stateChanged", listener);
  },
  onPlaySound: (callback: (sound: string) => void) => {
    const listener = (_event: any, sound: string) => callback(sound);
    ipcRenderer.on("play-sound", listener);
    return () => ipcRenderer.removeListener("play-sound", listener);
  },
  onShowNotification: (callback: (thumbnailUrl: string, position: string) => void) => {
    const listener = (_event: any, thumbnailUrl: string, position: string) => callback(thumbnailUrl, position);
    ipcRenderer.on("show-notification", listener);
    return () => ipcRenderer.removeListener("show-notification", listener);
  },
  hideNotification: () => ipcRenderer.send("hide-notification"),
  selectFolder: (currentPath: string) => ipcRenderer.invoke("dialog:selectFolder", currentPath)
};

contextBridge.exposeInMainWorld("clipture", api);
