import { inviteServer, type TelemetryApi, type TelemetryStatus } from '../../shared/telemetry';
import { hostKind } from './client';
import { createTauriTelemetryAdapter } from './tauri-telemetry-adapter';

const offStatus = (supported: boolean): TelemetryStatus => ({
  supported,
  state: 'off',
  server: null,
  friendName: null,
  deviceId: null,
  enrolledAt: null,
  pendingBytes: 0,
  lastUploadAt: null,
  lastUploadRecords: 0,
  lastError: null
});

/** Hosts other than Tauri have no telemetry client. */
function createUnsupportedTelemetryAdapter(): TelemetryApi {
  const unavailable = () => Promise.reject(new Error('Telemetry needs the Clipture desktop app.'));
  return {
    getStatus: async () => offStatus(false),
    enroll: unavailable,
    stop: unavailable,
    lastBatch: async () => []
  };
}

/** Browser preview: enrollment succeeds locally for any well-formed code. */
export function createMockTelemetryAdapter(): TelemetryApi {
  let status = offStatus(true);
  return {
    getStatus: async () => status,
    enroll: async (code) => {
      const server = inviteServer(code);
      if (!server) throw new Error('That is not a Clipture telemetry invite code.');
      status = { ...status, state: 'active', server, friendName: 'preview', deviceId: 'd-preview', enrolledAt: new Date().toISOString() };
      return status;
    },
    stop: async () => (status = offStatus(true)),
    lastBatch: async () => (status.state === 'active'
      ? [{ seq: 1, t: new Date().toISOString(), run: 'r-preview', session: null, type: 'event', level: 'info', code: 'enrolled' }]
      : [])
  };
}

function selectTelemetryClient(): TelemetryApi {
  if (hostKind === 'tauri') return createTauriTelemetryAdapter();
  if (hostKind === 'mock') return createMockTelemetryAdapter();
  return createUnsupportedTelemetryAdapter();
}

export const telemetry = selectTelemetryClient();
