import type { SharingApi } from '../../shared/sharing';
import { hostKind } from './client';
import { createMockSharingAdapter, mockSharingSeed, unsupportedSharingSnapshot } from './mock-sharing-adapter';
import { previewSearch } from './mockPreview';
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
    setFriendNickname: unavailable,
    removeFriend: unavailable,
    shareClip: unavailable,
    revokeShare: unavailable,
    dismissSharedClip: unavailable,
    answerSharedClip: unavailable,
    saveSharedClip: unavailable,
    cancelDownload: unavailable,
    streamUrl: unavailable,
    onChanged: () => () => {}
  };
}

function selectSharingClient(): SharingApi {
  if (hostKind === 'tauri') return createTauriSharingAdapter();
  if (hostKind === 'mock') {
    const seed = mockSharingSeed(previewSearch(window.location.search));
    return seed ? createMockSharingAdapter(seed) : createUnsupportedSharingAdapter();
  }
  return createUnsupportedSharingAdapter();
}

export const sharing = selectSharingClient();

/** False in the public web demo (clipture.app/demo): its friend code and
 * invite link are placeholders, and a copied link would send whoever opens it
 * a request to a friend who does not exist. */
export const sharingInvitesAvailable =
  hostKind !== 'mock' || !new URLSearchParams(previewSearch(window.location.search)).has('demo');
