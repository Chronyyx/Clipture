import type { CliptureApi } from '../../shared/types';
import { createElectronAdapter, hasElectronBridge } from './electron-adapter';
import type { HostKind } from './hostError';
import { createMockCliptureAdapter } from './mock-adapter';
import { createTauriAdapter, hasTauriRuntime } from './tauri-adapter';

function selectHost(): { kind: HostKind; client: CliptureApi } {
  if (hasTauriRuntime()) return { kind: 'tauri', client: createTauriAdapter() };
  if (hasElectronBridge()) return { kind: 'electron', client: createElectronAdapter() };
  return { kind: 'mock', client: createMockCliptureAdapter() };
}

const selected = selectHost();

export const hostKind = selected.kind;
export const clipture = selected.client;
export type { CliptureApi } from '../../shared/types';
