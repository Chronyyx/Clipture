import type { EngineDiagnostics } from '../../../shared/types';
import { defaultDiagnostics } from '../../shared/diagnostics/defaultDiagnostics';

export interface DiagnosticsSnapshot {
  diagnostics: EngineDiagnostics;
  received: boolean;
  error?: string;
}

export const initialDiagnosticsSnapshot: DiagnosticsSnapshot = {
  diagnostics: defaultDiagnostics,
  received: false
};

type SnapshotAction =
  | { type: 'received'; diagnostics: EngineDiagnostics }
  | { type: 'delayed'; error: unknown };

export function diagnosticsSnapshot(state: DiagnosticsSnapshot, action: SnapshotAction): DiagnosticsSnapshot {
  if (action.type === 'received') {
    // An actual degraded/offline report is authoritative, unlike a timeout.
    return { diagnostics: action.diagnostics, received: true };
  }
  const error = action.error instanceof Error ? action.error.message
    : typeof action.error === 'string' ? action.error : 'Diagnostics refresh failed.';
  return { ...state, error };
}
