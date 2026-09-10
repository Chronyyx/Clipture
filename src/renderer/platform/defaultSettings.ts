import type { ClipSettings } from "../../shared/types";

export const defaultSettings: ClipSettings = {
  uiTheme: "graphite",
  customMainColor: "#101114",
  customAccentColor: "#c8a6ff",
  clipLengthSeconds: 30,
  saveInPlace: true,
  fps: 30,
  bitrateMbps: 40,
  autoBitrate: false,
  maxAutoBitrateMbps: 80,
  nvencPreset: 3,
  resolutionPreset: "system",
  monitorMode: "primary",
  monitorId: "primary",
  startOnLogin: true,
  hotkey: "Ctrl+Shift+S",
  clipSound: "default.mp3",
  showNotification: true,
  notificationPosition: "top-right",
  saveFolder: "",
  importedVideoDirectories: [],
  importedVideoTitles: {},
  audioSources: [
    {
      id: "system",
      label: "System audio",
      kind: "system",
      captureAllSystem: true,
      enabled: true,
      omitIfSilent: true
    },
    {
      id: "mic",
      label: "Microphone",
      kind: "microphone",
      enabled: true,
      omitIfSilent: true,
      volume: 1,
      noiseGateEnabled: true,
      autoNoiseGate: true,
      noiseGateThreshold: 0.05,
      noiseGateDebounceMs: 180
    },
    {
      id: "game",
      label: "Detected game/app",
      kind: "game",
      enabled: false,
      omitIfSilent: true
    }
  ]
};
