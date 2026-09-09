import type { CliptureApi } from '../../shared/types';
import { mergeDiagnostics } from './diagnostics';
import { HostCapabilityError } from './hostError';
import { preserveSaveResult } from './save-result';

export function hasElectronBridge(): boolean {
  return typeof window.clipture === 'object' && window.clipture !== null;
}

export function createElectronAdapter(bridge: CliptureApi = window.clipture): CliptureApi {
  if (!bridge) {
    throw new HostCapabilityError('electron', 'bootstrap', 'window.clipture was not exposed by preload');
  }

  return {
    ...bridge,
    saveClip: async durationSeconds => preserveSaveResult(await bridge.saveClip(durationSeconds)),
    getDiagnostics: async () => mergeDiagnostics(await bridge.getDiagnostics())
  };
}
