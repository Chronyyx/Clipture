import { invoke } from '@tauri-apps/api/core';
import type { TelemetryApi, TelemetryRecord, TelemetryStatus } from '../../shared/telemetry';

export const telemetryCommands = {
  getStatus: 'telemetry_status',
  enroll: 'telemetry_enroll',
  stop: 'telemetry_stop',
  lastBatch: 'telemetry_last_batch'
} as const;

/** Telemetry errors are written for people, so they pass through as-is. */
function call<T>(capability: keyof typeof telemetryCommands, args?: Record<string, unknown>): Promise<T> {
  return invoke<T>(telemetryCommands[capability], args).catch((error: unknown) => {
    const detail = typeof error === 'string' ? error : error instanceof Error ? error.message : 'Telemetry is unavailable.';
    throw new Error(detail === 'UI host is busy; retry shortly' ? 'Clipture is busy. Try again in a moment.' : detail);
  });
}

export function createTauriTelemetryAdapter(): TelemetryApi {
  return {
    getStatus: () => call<TelemetryStatus>('getStatus'),
    enroll: (code) => call<TelemetryStatus>('enroll', { code }),
    stop: () => call<TelemetryStatus>('stop'),
    lastBatch: () => call<TelemetryRecord[]>('lastBatch')
  };
}
