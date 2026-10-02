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
    invite::{parse_invite, Invite},
    model::{Download, FriendStatus, NodeStatus},
    node::{friend_code, parse_friend_code, Network, Node},
    presence::PresenceBook,
    store::{StateFile, SHARE_WINDOW_MS},
    streams::{StreamRegistry, StreamTarget},
    transfers::TransferBook,
    wire,
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
    transfers: Arc<TransferBook>,
    /// How long friends may watch or start keeping a clip after accepting.
    /// Fixed outside tests.
    share_window_ms: Arc<AtomicU64>,
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
            transfers: Arc::default(),
            share_window_ms: Arc::new(AtomicU64::new(SHARE_WINDOW_MS)),
            pending_invite: Mutex::new(None),
        })
    }

    /// Tests use a short window instead of waiting fifteen minutes.
    #[cfg(test)]
    pub(super) fn set_share_window(&self, window_ms: u64) {
        self.share_window_ms.store(window_ms, std::sync::atomic::Ordering::Relaxed);
    }

    /// Starts the node at launch when the user previously turned sharing on.
    pub fn resume(self: &Arc<Self>) {
        self.tidy_copies();
        if self.core.read(|state| state.enabled) {
            self.start();
        }
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

    /// Removes a friend. They are told now if reachable; otherwise the
    /// goodbye waits in the store and goes out when they can hear it.
    pub fn remove_friend(self: &Arc<Self>, id: &str) -> AppResult<()> {
        self.core.update(|state| state.remove_friend(id))?;
        self.tidy_copies();
        let owed = self.core.read(|state| state.goodbyes.iter().any(|goodbye| goodbye == id));
        if let (true, Some(node)) = (owed, self.node()) {
            let this = self.clone();
            let id = id.to_owned();
            self.runtime.spawn(async move { this.send_goodbye(&node, &id).await });
        }
        Ok(())
    }

    /// Accepts or declines a clip a friend wants to send; they are told.
    /// Accepting only allows streaming and keeping; nothing transfers yet.
    pub fn answer_shared_clip(self: &Arc<Self>, share_id: &str, accept: bool) -> AppResult<()> {
        let friend_id = self.core.update(|state| state.answer_offer(share_id, accept, now_ms()))??;
        self.deliver_to_soon(friend_id);
        Ok(())
    }

    /// Removes a clip from the inbox; a kept library copy is untouched. A
    /// clip still waiting for an answer is declined, so the sender knows.
    pub fn dismiss_shared_clip(self: &Arc<Self>, share_id: &str) -> AppResult<()> {
        let awaiting = self.core.read(|state| {
            state
                .inbox_clip(share_id)
                .is_some_and(|clip| clip.awaiting_answer)
        });
        if awaiting {
            return self.answer_shared_clip(share_id, false);
        }
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
                    .filter(|clip| state.is_accepted(&clip.friend_id) && !clip.awaiting_answer)
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
#[path = "service_snapshot.rs"]
mod snapshot;

#[cfg(test)]
#[path = "service_tests.rs"]
mod tests;

#[cfg(test)]
#[path = "service_bench.rs"]
mod bench;
