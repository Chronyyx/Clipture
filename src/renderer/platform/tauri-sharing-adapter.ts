import { invoke } from '@tauri-apps/api/core';
import type { FriendStatus, SharedClipResult, SharingApi, SharingSnapshot } from '../../shared/sharing';
import { subscribeToTauriEvent } from './tauri-events';

export const sharingCommands = {
  getState: 'sharing_get_state',
  setEnabled: 'sharing_set_enabled',
  setAppearOffline: 'sharing_set_appear_offline',
  setDisplayName: 'sharing_set_display_name',
  addFriend: 'sharing_add_friend',
  acceptInvite: 'sharing_accept_invite',
  dismissInvite: 'sharing_dismiss_invite',
  acceptFriend: 'sharing_accept_friend',
  setFriendNickname: 'sharing_set_nickname',
  removeFriend: 'sharing_remove_friend',
  shareClip: 'sharing_share_clip',
  revokeShare: 'sharing_revoke_share',
  dismissSharedClip: 'sharing_dismiss_clip',
  answerSharedClip: 'sharing_answer_clip',
  saveSharedClip: 'sharing_save_clip',
  cancelDownload: 'sharing_cancel_download',
  streamUrl: 'sharing_stream_url'
} as const;

export const sharingChangedEvent = 'sharing://changed';

/** Sharing errors are written for people ("your friend is offline"), so
 * they are passed through without transport prefixes. */
function call<T>(capability: keyof typeof sharingCommands, args?: Record<string, unknown>): Promise<T> {
  return invoke<T>(sharingCommands[capability], args).catch((error: unknown) => {
    const detail = typeof error === 'string' ? error : error instanceof Error ? error.message : 'Sharing is unavailable.';
    const busy = detail === 'UI host is busy; retry shortly';
    throw new Error(busy ? 'Clipture is busy. Try again in a moment.' : capitalize(detail));
  });
}

function capitalize(message: string) {
  return message.charAt(0).toUpperCase() + message.slice(1) + (/[.!?]$/.test(message) ? '' : '.');
}

export function createTauriSharingAdapter(): SharingApi {
  return {
    getState: () => call<SharingSnapshot>('getState'),
    setEnabled: (enabled) => call<SharingSnapshot>('setEnabled', { enabled }),
    setAppearOffline: (appearOffline) => call<SharingSnapshot>('setAppearOffline', { appearOffline }),
    setDisplayName: (name) => call<SharingSnapshot>('setDisplayName', { name }),
    addFriend: (code, name) => call<FriendStatus>('addFriend', { code, name }),
    acceptInvite: () => call<SharingSnapshot>('acceptInvite'),
    dismissInvite: () => call<SharingSnapshot>('dismissInvite'),
    acceptFriend: (friendId) => call<SharingSnapshot>('acceptFriend', { friendId }),
    setFriendNickname: (friendId, nickname) => call<SharingSnapshot>('setFriendNickname', { friendId, nickname }),
    removeFriend: (friendId) => call<SharingSnapshot>('removeFriend', { friendId }),
    shareClip: (friendId, filePath) => call<SharedClipResult>('shareClip', { friendId, filePath }),
    answerSharedClip: (shareId, accept) => call<SharingSnapshot>('answerSharedClip', { shareId, accept }),
    revokeShare: (shareId) => call<SharingSnapshot>('revokeShare', { shareId }),
    dismissSharedClip: (shareId) => call<SharingSnapshot>('dismissSharedClip', { shareId }),
    saveSharedClip: (shareId) => call<SharingSnapshot>('saveSharedClip', { shareId }),
    cancelDownload: (shareId) => call<SharingSnapshot>('cancelDownload', { shareId }),
    streamUrl: async (shareId) => {
      const video = await call<string>('streamUrl', { shareId });
      return { video, audio: video.replace(/\/video$/, '/audio') };
    },
    onChanged: (callback) => subscribeToTauriEvent<unknown>(sharingChangedEvent, () => callback())
  };
}
