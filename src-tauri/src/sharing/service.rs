//! Friend-to-friend clip sharing. Owned by the resident controller, so friend
//! requests, deliveries and "add to library" downloads continue while the
//! WebView is closed. Commands call this facade; it never sees Tauri types.
use std::{
    fs,
    io::Read,
    path::PathBuf,
    sync::{atomic::AtomicU64, Arc, Mutex, OnceLock, RwLock},
    time::Duration,
};

use iroh::{EndpointId, SecretKey};

use crate::{
    contracts::ClipRecord,
    error::{AppError, AppResult},
    media::{RemoteMediaFile, RemoteMediaSource, RemoteVideoChunk},
};

use super::{
    core::{now_ms, Core, SharingEvents},
    download::partial_path,
    invite::{invite_link, parse_invite, Invite},
    model::{
        ClipOffer, Download, FriendStatus, FriendView, InviteView, NodeStatus,
        Presence, SharedClipView, SharingSnapshot,
    },
    node::{friend_code, parse_friend_code, Network, Node},
    presence::PresenceBook,
    store::StateFile,
    streams::{StreamRegistry, StreamTarget},
    wire::{self, Request},
};

const PROGRESS_INTERVAL: Duration = Duration::from_millis(250);
const MAX_DOWNLOAD_ROWS: usize = 20;

/// Where kept clips go and how they join the library.
pub trait ClipLibrary: Send + Sync {
    fn shared_clips_folder(&self) -> PathBuf;
    fn publish(&self, record: ClipRecord) -> AppResult<()>;
    /// Where stream copies of clips we share are kept.
    fn outgoing_copies_folder(&self) -> PathBuf;
    /// Writes a stream-friendly copy of `source` to `destination`; `false`
    /// (writing nothing) when the clip already streams well as stored.
    fn write_stream_copy(&self, source: &std::path::Path, destination: &std::path::Path) -> AppResult<bool>;
}

/// A clip from the local library, already authorized by the caller.
pub struct ShareSource {
    pub path: PathBuf,
    pub title: String,
    pub duration_seconds: u32,
    pub resolution: String,
    pub game_or_app: String,
    pub fps: u32,
    pub audio_tracks: Vec<String>,
}

pub struct SharingService {
    core: Arc<Core>,
    library: Arc<dyn ClipLibrary>,
    runtime: tokio::runtime::Handle,
    network: Network,
    identity: OnceLock<SecretKey>,
    node: RwLock<Option<Arc<Node>>>,
    status: Mutex<(NodeStatus, Option<String>)>,
    generation: AtomicU64,
    downloads: Mutex<Vec<Download>>,
    streams: Arc<StreamRegistry>,
    presence: Arc<PresenceBook>,
    /// At most one link waits for a decision; a newer link replaces it.
    pending_invite: Mutex<Option<Invite>>,
}

impl SharingService {
    pub fn new(
        directory: PathBuf,
        events: Box<dyn SharingEvents>,
        library: Arc<dyn ClipLibrary>,
        runtime: tokio::runtime::Handle,
        network: Network,
    ) -> Arc<Self> {
        let core = Arc::new(Core::load(StateFile::new(directory), events));
        let progress_core = core.clone();
        Arc::new(Self {
            core,
            library,
            runtime,
            network,
            identity: OnceLock::new(),
            node: RwLock::new(None),
            status: Mutex::new((NodeStatus::Off, None)),
            generation: AtomicU64::new(0),
            downloads: Mutex::new(Vec::new()),
            streams: Arc::new(StreamRegistry::new(Box::new(move || {
                progress_core.events.changed()
            }))),
            presence: Arc::default(),
            pending_invite: Mutex::new(None),
        })
    }

    /// Starts the node at launch when the user previously turned sharing on.
    pub fn resume(self: &Arc<Self>) {
        self.tidy_copies();
        if self.core.read(|state| state.enabled) {
            self.start();
        }
    }

    pub fn snapshot(&self) -> SharingSnapshot {
        let (status, status_message) = self.status();
        let friend_code = self.identity.get().map(|key| friend_code(&key.public()));
        let downloads = self.lock_downloads().clone();
        let pending_invite = self
            .pending_invite
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone();
        let mut snapshot = self.core.read(|state| {
            let name_of = |id: &str| {
                state
                    .friend(id)
                    .map(|friend| friend.name.clone())
                    .unwrap_or_else(|| "Former friend".into())
            };
            let mut inbox: Vec<_> = state
                .inbox
                .iter()
                .map(|clip| {
                    view(
                        &clip.offer,
                        &clip.friend_id,
                        name_of(&clip.friend_id),
                        clip.received_at_ms,
                        clip.saved_path.is_some(),
                        true,
                    )
                })
                .collect();
            let mut outbox: Vec<_> = state
                .outbox
                .iter()
                .map(|share| {
                    view(
                        &share.offer,
                        &share.friend_id,
                        name_of(&share.friend_id),
                        share.offer.created_at_ms,
                        false,
                        share.delivered,
                    )
                })
                .collect();
            inbox.reverse();
            outbox.reverse();
            SharingSnapshot {
                supported: true,
                enabled: state.enabled,
                appear_offline: state.appear_offline,
                status,
                status_message,
                invite_link: friend_code
                    .as_deref()
                    .filter(|_| state.enabled)
                    .map(|code| invite_link(code, &state.display_name)),
                pending_invite: pending_invite.map(|invite| InviteView {
                    already_friends: state.is_accepted(&invite.code),
                    code: invite.code,
                    name: invite.name,
                }),
                friend_code: friend_code.filter(|_| state.enabled),
                display_name: state.display_name.clone(),
                friends: state
                    .friends
                    .iter()
                    .map(|friend| FriendView {
                        friend: friend.clone(),
                        presence: if friend.status != FriendStatus::Accepted {
                            Presence::Offline
                        } else if state.appear_offline || status != NodeStatus::Online {
                            Presence::Unknown
                        } else if self.presence.is_online(&friend.id) {
                            Presence::Online
                        } else {
                            Presence::Offline
                        },
                    })
                    .collect(),
                inbox,
                outbox,
                downloads,
            }
        });
        // Outside the state lock: stream sessions have their own.
        for clip in &mut snapshot.inbox {
            clip.streamed = self.streams.arrived(&clip.share_id);
            clip.all_audio_ready = self.streams.complete(&clip.share_id);
        }
        snapshot
    }

    pub fn set_enabled(self: &Arc<Self>, enabled: bool) -> AppResult<()> {
        self.core.update(|state| state.enabled = enabled)?;
        if enabled {
            self.start();
        } else {
            self.stop();
        }
        Ok(())
    }

    /// Appear offline: announce departure, then restart the node so it no
    /// longer publishes its address and refuses every incoming connection.
    /// Friends keep what they send and deliver it when we are visible again.
    pub fn set_appear_offline(self: &Arc<Self>, appear_offline: bool) -> AppResult<()> {
        let changed = self.core.update_when(|state| {
            let changed = state.appear_offline != appear_offline;
            state.appear_offline = appear_offline;
            (changed, changed)
        })?;
        if changed && self.core.read(|state| state.enabled) {
            self.stop();
            self.start();
        }
        Ok(())
    }

    pub fn set_display_name(&self, name: &str) -> AppResult<()> {
        let name = wire::clean_name(name);
        if name.is_empty() {
            return Err(AppError::Path("choose a name your friends will see".into()));
        }
        self.core.update(|state| state.display_name = name)
    }

    /// Accepts a friend code or any invite link form.
    pub fn add_friend(self: &Arc<Self>, code: &str, name: &str) -> AppResult<FriendStatus> {
        let invite = parse_invite(code)
            .ok_or_else(|| AppError::Path("that friend code or invite link is not valid".into()))?;
        let peer = parse_friend_code(&invite.code)?;
        let name = if name.trim().is_empty() {
            invite.name.as_str()
        } else {
            name
        };
        if self.identity.get().is_some_and(|key| key.public() == peer) {
            return Err(AppError::Path("that is your own friend code".into()));
        }
        let name = wire::clean_name(name);
        let status = self
            .core
            .update(|state| state.add_friend(&friend_code(&peer), &name, now_ms()))??;
        self.deliver_soon();
        Ok(status)
    }

    /// An invite link opened Clipture. Nothing happens until the user
    /// confirms; links to ourselves are ignored.
    pub fn receive_invite(&self, invite: Invite) {
        let own = self
            .identity()
            .is_ok_and(|key| friend_code(&key.public()) == invite.code);
        if own {
            return;
        }
        *self
            .pending_invite
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(invite);
        self.core.events.changed();
    }

    /// Adds the friend from the pending invite, turning sharing on first.
    pub fn accept_invite(self: &Arc<Self>) -> AppResult<FriendStatus> {
        let invite = self
            .pending_invite
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .take()
            .ok_or_else(|| AppError::Path("that invite is no longer waiting".into()))?;
        if !self.core.read(|state| state.enabled) {
            self.set_enabled(true)?;
        }
        let status = self.add_friend(&invite.code, &invite.name)?;
        self.core.events.changed();
        Ok(status)
    }

    pub fn dismiss_invite(&self) {
        let removed = self
            .pending_invite
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .take()
            .is_some();
        if removed {
            self.core.events.changed();
        }
    }

    pub fn accept_friend(self: &Arc<Self>, id: &str) -> AppResult<()> {
        self.core.update(|state| state.accept_request(id))??;
        self.deliver_soon();
        Ok(())
    }

    pub fn remove_friend(self: &Arc<Self>, id: &str) -> AppResult<()> {
        let was_connected = self.core.read(|state| {
            state
                .friend(id)
                .is_some_and(|friend| friend.status != FriendStatus::Incoming)
        });
        self.core.update(|state| state.remove_friend(id))?;
        self.tidy_copies();
        if was_connected {
            if let (Ok(peer), Some(node)) = (parse_friend_code(id), self.node()) {
                self.runtime.spawn(async move {
                    let _ = node.notify(peer, &Request::Goodbye {}).await;
                });
            }
        }
        Ok(())
    }

    /// Removes a clip from the inbox; a kept library copy is untouched.
    pub fn dismiss_shared_clip(&self, share_id: &str) -> AppResult<()> {
        self.cancel_download(share_id);
        self.core
            .update(|state| state.inbox.retain(|clip| clip.offer.share_id != share_id))
    }

    /// Returns an opaque stream id for the media protocol.
    pub fn open_stream(&self, owner: &str, share_id: &str) -> AppResult<String> {
        self.require_node()?;
        let (friend_id, size, saved, audio_tracks) = self
            .core
            .read(|state| {
                state
                    .inbox_clip(share_id)
                    .filter(|clip| state.is_accepted(&clip.friend_id))
                    .map(|clip| {
                        (
                            clip.friend_id.clone(),
                            clip.offer.size,
                            clip.saved_path.clone(),
                            clip.offer.audio_tracks.clone(),
                        )
                    })
            })
            .ok_or_else(|| AppError::Path("this shared clip is no longer available".into()))?;
        let peer = parse_friend_code(&friend_id)?;
        Ok(self.streams.open(
            &self.runtime,
            self.require_node()?,
            owner,
            StreamTarget {
                share_id: share_id.into(),
                peer,
                size,
                local: saved
                    .map(PathBuf::from)
                    .into_iter()
                    .chain([partial_path(&self.library.shared_clips_folder(), share_id)])
                    .collect(),
                audio_tracks,
            },
        ))
    }
}

impl RemoteMediaSource for SharingService {
    fn read_video(
        &self,
        stream_id: &str,
        owner: &str,
        range_header: Option<&str>,
    ) -> AppResult<RemoteVideoChunk> {
        let node = self.require_node()?;
        self.streams
            .read(&self.runtime, node, stream_id, owner, range_header)
    }

    fn complete_file(&self, stream_id: &str, owner: &str) -> AppResult<RemoteMediaFile> {
        let (path, audio_tracks) = self.streams.complete_file(stream_id, owner)?;
        Ok(RemoteMediaFile { path, audio_tracks })
    }

    fn release_owner(&self, owner: &str) -> usize {
        self.streams.release_owner(owner)
    }
}

fn view(
    offer: &ClipOffer,
    friend_id: &str,
    friend_name: String,
    shared_at_ms: u64,
    saved: bool,
    delivered: bool,
) -> SharedClipView {
    SharedClipView {
        share_id: offer.share_id.clone(),
        friend_id: friend_id.into(),
        friend_name,
        title: offer.title.clone(),
        size: offer.size,
        duration_seconds: offer.duration_seconds,
        resolution: offer.resolution.clone(),
        game_or_app: offer.game_or_app.clone(),
        created_at_ms: offer.created_at_ms,
        shared_at_ms,
        saved,
        delivered,
        audio_tracks: offer.audio_tracks.clone(),
        streamed: Vec::new(),
        all_audio_ready: false,
    }
}

fn hash_file(path: &std::path::Path) -> AppResult<(u64, String)> {
    let mut file = fs::File::open(path).map_err(|source| AppError::Io {
        action: "open clip to share",
        path: path.to_owned(),
        source,
    })?;
    let mut hasher = blake3::Hasher::new();
    let mut buffer = vec![0_u8; 1024 * 1024];
    let mut size = 0_u64;
    loop {
        let read = file.read(&mut buffer).map_err(|source| AppError::Io {
            action: "read clip to share",
            path: path.to_owned(),
            source,
        })?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
        size += read as u64;
    }
    Ok((size, hasher.finalize().to_hex().to_string()))
}

/// Unused ids are rejected by the store; this only guards the command layer.
pub fn is_friend_id(value: &str) -> bool {
    parse_friend_code(value).is_ok_and(|id: EndpointId| friend_code(&id) == value)
}

#[path = "service_keep.rs"]
mod keep;
#[path = "service_lifecycle.rs"]
mod lifecycle;
#[path = "service_outgoing.rs"]
mod outgoing;

#[cfg(test)]
#[path = "service_tests.rs"]
mod tests;

#[cfg(test)]
#[path = "service_bench.rs"]
mod bench;
