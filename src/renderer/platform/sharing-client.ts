import type { SharingApi } from '../../shared/sharing';
import { hostKind } from './client';
import { createMockSharingAdapter, mockSharingSeed, unsupportedSharingSnapshot } from './mock-sharing-adapter';
import { createTauriSharingAdapter } from './tauri-sharing-adapter';

/** Hosts other than Tauri have no peer node; they report `supported: false`
 * and reject actions, so the Friends tab can explain instead of failing. */
function createUnsupportedSharingAdapter(): SharingApi {
  const unavailable = () => Promise.reject(new Error('Friend sharing needs the Clipture desktop app.'));
  return {
    getState: async () => unsupportedSharingSnapshot(),
    setEnabled: unavailable,
    setAppearOffline: unavailable,
    setDisplayName: unavailable,
    addFriend: unavailable,
    acceptInvite: unavailable,
    dismissInvite: unavailable,
    acceptFriend: unavailable,
    removeFriend: unavailable,
    shareClip: unavailable,
    revokeShare: unavailable,
    dismissSharedClip: unavailable,
    saveSharedClip: unavailable,
    cancelDownload: unavailable,
    streamUrl: unavailable,
    onChanged: () => () => {}
  };
}

function selectSharingClient(): SharingApi {
  if (hostKind === 'tauri') return createTauriSharingAdapter();
  if (hostKind === 'mock') {
    const seed = mockSharingSeed(window.location.search);
    return seed ? createMockSharingAdapter(seed) : createUnsupportedSharingAdapter();
  }
  return createUnsupportedSharingAdapter();
}

export const sharing = selectSharingClient();
