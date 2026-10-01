/**
 * Friend-to-friend clip sharing. A separate, additive client beside the v1
 * `CliptureApi` facade (see docs/adr/0011-p2p-clip-sharing.md). Only the Tauri
 * host implements it; other hosts report `supported: false`.
 */
export type FriendStatus = "accepted" | "outgoing" | "incoming";
export type SharingNodeStatus = "off" | "starting" | "online" | "error";
export type SharingDownloadPhase = "running" | "done" | "failed";
/** `unknown` while you appear offline or are not connected. */
export type FriendPresence = "online" | "offline" | "unknown";

export interface Friend {
  /** The friend's public key, which is also their friend code. */
  id: string;
  name: string;
  status: FriendStatus;
  addedAtMs: number;
  /** Our request/acceptance has not reached them yet. */
  undelivered: boolean;
  presence: FriendPresence;
}

export interface SharedClip {
  shareId: string;
  friendId: string;
  friendName: string;
  title: string;
  size: number;
  durationSeconds: number;
  resolution: string;
  gameOrApp: string;
  createdAtMs: number;
  sharedAtMs: number;
  /** Inbox: a verified copy is in the library. */
  saved: boolean;
  /** Outbox: the friend has been told about it. */
  delivered: boolean;
  audioTracks: string[];
  /** Inbox: byte ranges `[start, end)` streamed so far, sorted and merged. */
  streamed: [number, number][];
  /** Inbox: the whole clip is here, so every audio track can play. */
  allAudioReady: boolean;
}

/** Where a friend's clip streams from. */
export interface StreamUrls {
  video: string;
  /** Mixed audio of every track, available once the clip has fully arrived. */
  audio: string;
}

/** An invite link that opened Clipture and waits for the user to decide. */
export interface PendingInvite {
  code: string;
  /** Chosen by whoever made the link, so it is only a suggestion. */
  name: string;
  alreadyFriends: boolean;
}

export interface SharingDownload {
  shareId: string;
  receivedBytes: number;
  totalBytes: number;
  phase: SharingDownloadPhase;
  message?: string | null;
  bytesPerSecond: number;
  /** Through a relay (slower) rather than directly; null until known. */
  relayed: boolean | null;
  /** The connection dropped; it resumes from the bytes already received. */
  reconnecting: boolean;
}

export interface SharingSnapshot {
  supported: boolean;
  enabled: boolean;
  /** Nothing is received and friends see you as offline. */
  appearOffline: boolean;
  status: SharingNodeStatus;
  statusMessage?: string | null;
  friendCode?: string | null;
  /** A clickable https link carrying your friend code and name. */
  inviteLink?: string | null;
  pendingInvite?: PendingInvite | null;
  displayName: string;
  friends: Friend[];
  inbox: SharedClip[];
  outbox: SharedClip[];
  downloads: SharingDownload[];
}

export interface SharingApi {
  getState(): Promise<SharingSnapshot>;
  setEnabled(enabled: boolean): Promise<SharingSnapshot>;
  /** While on, all incoming connections are refused; friends queue what
   * they send and deliver it once you are visible again. */
  setAppearOffline(appearOffline: boolean): Promise<SharingSnapshot>;
  setDisplayName(name: string): Promise<SharingSnapshot>;
  /** `code` may be a friend code or any invite link. */
  addFriend(code: string, name: string): Promise<FriendStatus>;
  /** Adds the friend from the pending invite, turning sharing on if needed. */
  acceptInvite(): Promise<SharingSnapshot>;
  dismissInvite(): Promise<SharingSnapshot>;
  acceptFriend(friendId: string): Promise<SharingSnapshot>;
  /** Removes a friend, or declines (and blocks) a pending request. */
  removeFriend(friendId: string): Promise<SharingSnapshot>;
  /** `filePath` must be a clip from the current library listing. */
  shareClip(friendId: string, filePath: string): Promise<SharingSnapshot>;
  revokeShare(shareId: string): Promise<SharingSnapshot>;
  dismissSharedClip(shareId: string): Promise<SharingSnapshot>;
  /** Starts a verified copy into the library; progress arrives via snapshots. */
  saveSharedClip(shareId: string): Promise<SharingSnapshot>;
  /** Stops an "add to library" transfer and discards the partial file. */
  cancelDownload(shareId: string): Promise<SharingSnapshot>;
  /** An opaque media URL that streams the clip without saving it. */
  streamUrl(shareId: string): Promise<StreamUrls>;
  /** A hint that state changed; re-read with `getState`. */
  onChanged(callback: () => void): () => void;
}
