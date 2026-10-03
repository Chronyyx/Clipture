//! Renderer-facing and persisted sharing records. Local file paths of shared
//! clips never appear in a renderer snapshot or on the wire.
use std::path::PathBuf;

use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub enum FriendStatus {
    /// Both sides agreed; clips may be offered and streamed.
    Accepted,
    /// We sent a request and wait for the peer to accept.
    Outgoing,
    /// The peer asked to be friends; nothing is accepted from them yet.
    Incoming,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct Friend {
    /// The peer's public key (z-base-32 friend code).
    pub id: String,
    pub name: String,
    pub status: FriendStatus,
    pub added_at_ms: u64,
    /// Our request has not reached the peer yet (it was offline).
    #[serde(default)]
    pub undelivered: bool,
    /// Our own name for them. It replaces `name` everywhere we show them,
    /// is never sent to anyone, and survives whatever name they pick.
    #[serde(default)]
    pub nickname: Option<String>,
}

impl Friend {
    /// What we call them: our nickname, or else the name they chose.
    pub fn display_name(&self) -> &str {
        self.nickname.as_deref().unwrap_or(&self.name)
    }
}

/// Metadata a sender publishes about one clip. Validated on receipt.
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct ClipOffer {
    pub share_id: String,
    pub title: String,
    pub size: u64,
    /// Lower-case hex BLAKE3 digest of the complete file.
    pub blake3: String,
    pub duration_seconds: u32,
    pub resolution: String,
    pub game_or_app: String,
    pub created_at_ms: u64,
    #[serde(default)]
    pub fps: u32,
    /// Stream labels in file order, so a kept copy can use the track mixer.
    #[serde(default)]
    pub audio_tracks: Vec<String>,
}

/// A clip we shared, kept so the recipient can stream it later.
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OutgoingShare {
    pub offer: ClipOffer,
    pub friend_id: String,
    /// The file served: the clip, or (shared by builds before 1.6.5) its
    /// stream copy.
    pub path: PathBuf,
    pub delivered: bool,
    /// The library clip, when `path` is a stream copy of it.
    #[serde(default)]
    pub source: Option<PathBuf>,
    /// The library clip's digest when shared, to spot edits on re-sharing.
    #[serde(default)]
    pub source_blake3: Option<String>,
    /// `path` is served in playing order through its linear view (see
    /// `media::LinearView`); the offer's size and digest are the view's.
    #[serde(default)]
    pub linear: bool,
    /// What the friend decided. Shares from before friends could decline
    /// were already in their inbox, so they read as accepted.
    #[serde(default = "accepted")]
    pub answer: ShareAnswer,
    /// Every byte has been read by the friend at least once.
    #[serde(default)]
    pub received_whole: bool,
    /// The friend confirmed a verified copy in their library. The share is
    /// closed: they have it, so it is never served again.
    #[serde(default)]
    pub kept: bool,
    /// When the friend accepted; their window to watch or keep starts here.
    #[serde(default)]
    pub accepted_at_ms: Option<u64>,
    /// A download began inside the window, so it may finish after it.
    #[serde(default)]
    pub keep_started: bool,
    /// The friend deleted it after accepting (`answer` is then declined).
    #[serde(default)]
    pub removed: bool,
}

fn accepted() -> ShareAnswer {
    ShareAnswer::Accepted
}

/// A friend's decision about a clip offered to them.
#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub enum ShareAnswer {
    Pending,
    Accepted,
    Declined,
}

/// Our decision about a friend's clip, kept until it reaches them.
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct PendingAnswer {
    pub friend_id: String,
    pub share_id: String,
    pub accepted: bool,
    /// Also confirms a verified copy is in our library.
    #[serde(default)]
    pub kept: bool,
    /// We deleted a clip we had accepted.
    #[serde(default)]
    pub removed: bool,
}

impl OutgoingShare {
    /// The library clip this share came from.
    pub fn original(&self) -> &std::path::Path {
        self.source.as_deref().unwrap_or(&self.path)
    }

    pub fn source_digest(&self) -> &str {
        self.source_blake3.as_deref().unwrap_or(&self.offer.blake3)
    }
}

/// A clip a friend shared with us. It streams until added to the library.
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InboxClip {
    pub offer: ClipOffer,
    pub friend_id: String,
    pub received_at_ms: u64,
    #[serde(default)]
    pub saved_path: Option<String>,
    /// Waiting for the user to accept or decline; nothing streams until then.
    /// Clips received before friends could decline read as accepted.
    #[serde(default)]
    pub awaiting_answer: bool,
    /// When we accepted it, to show how long it stays on the sender's PC.
    #[serde(default)]
    pub accepted_at_ms: Option<u64>,
}

#[derive(Clone, Copy, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub enum Presence {
    Online,
    Offline,
    /// Not knowable while we appear offline or are not connected.
    Unknown,
}

/// A friend as the UI sees them: the stored record plus live presence.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FriendView {
    #[serde(flatten)]
    pub friend: Friend,
    pub presence: Presence,
}

#[derive(Clone, Copy, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub enum NodeStatus {
    Off,
    Starting,
    Online,
    Error,
}

#[derive(Clone, Copy, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub enum DownloadPhase {
    Running,
    Done,
    Failed,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Download {
    pub share_id: String,
    pub received_bytes: u64,
    pub total_bytes: u64,
    pub phase: DownloadPhase,
    pub message: Option<String>,
    pub bytes_per_second: u64,
    /// Through a relay (slower) rather than directly; `None` until known.
    pub relayed: Option<bool>,
    /// The connection dropped; resuming from the bytes already received.
    pub reconnecting: bool,
    /// Stops the transfer; dropping it deletes the partial file.
    #[serde(skip)]
    pub abort: Option<tokio::task::AbortHandle>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SharedClipView {
    pub share_id: String,
    pub friend_id: String,
    pub friend_name: String,
    pub title: String,
    pub size: u64,
    pub duration_seconds: u32,
    pub resolution: String,
    pub game_or_app: String,
    pub created_at_ms: u64,
    pub shared_at_ms: u64,
    /// Inbox: a verified copy is in the library. Outbox: the friend
    /// confirmed theirs, and the share is closed.
    pub saved: bool,
    /// Outbox only: whether the friend has been told about it.
    pub delivered: bool,
    pub audio_tracks: Vec<String>,
    /// Inbox only: byte ranges `[start, end)` streamed so far.
    pub streamed: Vec<[u64; 2]>,
    /// Inbox only: the whole clip is here, so every audio track can play.
    pub all_audio_ready: bool,
    /// Inbox: `pending` until the user decides. Outbox: the friend's answer.
    pub answer: ShareAnswer,
    /// Outbox only: what the friend has read of the clip.
    pub transfer: Option<TransferView>,
    /// After this the sender serves nothing new: no watching, and no
    /// download that had not started. `None` until accepted.
    pub available_until_ms: Option<u64>,
    /// Outbox only: the friend deleted it after accepting.
    pub removed: bool,
}

#[derive(Clone, Copy, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub enum TransferPurpose {
    Watch,
    Keep,
}

#[derive(Clone, Copy, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub enum TransferState {
    /// Bytes are leaving now.
    Active,
    /// Started, then stopped by the friend (closed the player, cancelled).
    Paused,
    /// The connection closed in the middle of sending.
    Interrupted,
    /// The friend has read every byte.
    Complete,
}

/// The sender's view of one share: bytes that have left, by any stream.
#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct TransferView {
    pub sent_bytes: u64,
    pub total_bytes: u64,
    pub purpose: TransferPurpose,
    pub state: TransferState,
    pub bytes_per_second: u64,
}

/// An invite link that opened Clipture and awaits the user's decision.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InviteView {
    pub code: String,
    /// The name the link suggests; the sender chose it, so it is unverified.
    pub name: String,
    pub already_friends: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SharingSnapshot {
    pub supported: bool,
    pub enabled: bool,
    pub appear_offline: bool,
    pub status: NodeStatus,
    pub status_message: Option<String>,
    pub friend_code: Option<String>,
    /// A clickable link containing our friend code and name.
    pub invite_link: Option<String>,
    pub pending_invite: Option<InviteView>,
    pub display_name: String,
    pub friends: Vec<FriendView>,
    pub inbox: Vec<SharedClipView>,
    pub outbox: Vec<SharedClipView>,
    pub downloads: Vec<Download>,
}
