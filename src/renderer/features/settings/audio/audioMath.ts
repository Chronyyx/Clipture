export const maxManualNoiseGateThreshold = 0.2;
export const visualizerMinDb = -60;
export const visualizerMaxDb = 20 * Math.log10(maxManualNoiseGateThreshold);
export const minMicGainDb = -60;
export const maxMicGainDb = 25;

export function visualizerLevelFromRms(rms: number) {
  if (rms <= 0) return 0;
  const db = 20 * Math.log10(rms);
  return Math.min(1, Math.max(0, (db - visualizerMinDb) / (visualizerMaxDb - visualizerMinDb)));
}

export function rmsFromVisualizerLevel(level: number) {
  const boundedLevel = Math.min(1, Math.max(0, level));
  const db = visualizerMinDb + boundedLevel * (visualizerMaxDb - visualizerMinDb);
  return Math.pow(10, db / 20);
}

export function gainToDb(gain: number) {
  if (gain <= 0) return minMicGainDb;
  return Math.min(maxMicGainDb, Math.max(minMicGainDb, Math.round(20 * Math.log10(gain) * 2) / 2));
}

export function dbToGain(db: number) {
  if (db <= minMicGainDb) return 0;
  return Number(Math.pow(10, db / 20).toFixed(4));
}

export function formatDb(db: number) {
  if (db <= minMicGainDb) return "-inf dB";
  if (db > 0) return `+${db} dB`;
  return `${db} dB`;
}

export function sensitivityFromThreshold(threshold: number) {
  const visualizerLevel = visualizerLevelFromRms(threshold);
  return Math.round((1 - visualizerLevel) * 100 * 2) / 2;
}

export function thresholdFromSensitivity(sensitivity: number) {
  const boundedSensitivity = Math.min(100, Math.max(0, sensitivity));
  return Number(rmsFromVisualizerLevel(1 - boundedSensitivity / 100).toFixed(4));
}
