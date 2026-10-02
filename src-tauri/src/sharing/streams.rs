//! Streaming playback of a friend's clip. The player reads byte ranges from an
//! opaque, owner-bound session; runways (see `streams_runway.rs`) fetch the
//! clip in order, prioritising the positions the player is waiting on.
//!
//! Bytes go to a temporary file deleted with the session (`streams_store.rs`),
//! so clips of any size stay whole while watched; nothing reaches the
//! library until "Add to library". Blocks an "Add to library" copy already
//! holds are read from that copy instead of fetched again.
use std::{
    collections::HashMap,
    io::SeekFrom,
    path::PathBuf,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use iroh::EndpointId;
use tokio::io::{AsyncReadExt, AsyncSeekExt};

use crate::{
    error::{AppError, AppResult},
    media::{random_token, resolve_range, ByteRange, RemoteVideoChunk},
};

use super::node::Node;

#[path = "streams_order.rs"]
mod order;
#[path = "streams_runway.rs"]
mod runway;
#[path = "streams_store.rs"]
mod store;
use runway::{wait_for, Fetch, Shared};
use store::TEMP_PREFIX;

pub const BLOCK_BYTES: u64 = 1024 * 1024;
/// One player per window; a few windows at most.
const MAX_SESSIONS: usize = 3;
const SESSION_TTL: Duration = Duration::from_secs(30 * 60);
/// Largest reply once bytes are here: big strides for the player, but under
/// the 4 MiB the UI process pipe carries per message (with room to spare).
const MAX_RESPONSE_BLOCKS: u64 = 3;

pub struct StreamTarget {
    pub share_id: String,
    pub peer: EndpointId,
    pub size: u64,
    /// Local copies to read from first: the kept clip, then the partial
    /// file of a running or interrupted "Add to library".
    pub local: Vec<PathBuf>,
    pub audio_tracks: Vec<String>,
}

impl StreamTarget {
    fn blocks(&self) -> u64 {
        self.size.div_ceil(BLOCK_BYTES)
    }

    fn block_len(&self, index: u64) -> u64 {
        BLOCK_BYTES.min(self.size - index * BLOCK_BYTES)
    }
}

struct Session {
    owner: String,
    target: Arc<StreamTarget>,
    shared: Arc<Shared>,
    touched: Instant,
}

impl Drop for Session {
    fn drop(&mut self) {
        for runway in self.shared.lock().runways.drain(..) {
            runway.abort.abort();
        }
    }
}

pub struct StreamRegistry {
    sessions: Mutex<HashMap<String, Session>>,
    on_progress: Box<dyn Fn() + Send + Sync>,
}

impl StreamRegistry {
    /// `on_progress` is called (throttled) as bytes arrive, so the UI can
    /// draw what has been streamed.
    pub fn new(on_progress: Box<dyn Fn() + Send + Sync>) -> Self {
        remove_leftover_copies();
        Self {
            sessions: Mutex::default(),
            on_progress,
        }
    }

    /// Opens a session. Nothing is fetched until the player reads: guessing
    /// at the start of the file wastes the upload on clips whose frames begin
    /// elsewhere (measured: first frame 11.9 s -> 10.5 s on such a clip).
    pub fn open(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Handle,
        node: Arc<Node>,
        owner: &str,
        target: StreamTarget,
    ) -> String {
        let fetch = Fetch {
            runtime: runtime.clone(),
            node,
            target: Arc::new(target),
            shared: Arc::new(Shared::new()),
            progress: Arc::downgrade(self),
        };
        let id = random_token();
        {
            let mut sessions = self.lock();
            sessions.retain(|_, session| session.touched.elapsed() < SESSION_TTL);
            // A window plays one clip at a time; opening another replaces it.
            sessions.retain(|_, session| session.owner != owner);
            while sessions.len() >= MAX_SESSIONS {
                let Some(oldest) = sessions
                    .iter()
                    .min_by_key(|(_, session)| session.touched)
                    .map(|(id, _)| id.clone())
                else {
                    break;
                };
                sessions.remove(&oldest);
            }
            sessions.insert(
                id.clone(),
                Session {
                    owner: owner.into(),
                    target: fetch.target.clone(),
                    shared: fetch.shared.clone(),
                    touched: Instant::now(),
                },
            );
        }
        id
    }

    /// Bytes streamed so far for a clip, as sorted, merged `[start, end)`.
    pub fn arrived(&self, share_id: &str) -> Vec<[u64; 2]> {
        let mut ranges: Vec<[u64; 2]> = Vec::new();
        for session in self.lock().values().filter(|s| s.target.share_id == share_id) {
            ranges.extend(session.shared.lock().store.ranges());
        }
        ranges.sort_unstable();
        ranges.into_iter().fold(Vec::new(), |mut merged, range| {
            match merged.last_mut() {
                Some(last) if last[1] >= range[0] => last[1] = last[1].max(range[1]),
                _ => merged.push(range),
            }
            merged
        })
    }

    /// Whether every audio track of the clip can be mixed now: a session has
    /// the whole clip, or a kept copy is in the library.
    pub fn complete(&self, share_id: &str) -> bool {
        self.lock()
            .values()
            .filter(|session| session.target.share_id == share_id)
            .any(|session| {
                session.shared.lock().store.count() == session.target.blocks()
                    || local_blocks(&session.target) == session.target.blocks()
            })
    }

    /// The whole clip as a local file, for the owner's stream session.
    pub fn complete_file(
        &self,
        stream_id: &str,
        owner: &str,
    ) -> AppResult<(PathBuf, Vec<String>)> {
        let sessions = self.lock();
        let session = sessions
            .get(stream_id)
            .filter(|session| session.owner == owner)
            .ok_or_else(|| AppError::Path("stream session is unavailable".into()))?;
        let target = &session.target;
        let kept = target.local.iter().find(|path| {
            std::fs::metadata(path).is_ok_and(|metadata| metadata.len() == target.size)
        });
        let path = match kept {
            Some(path) => path.clone(),
            None => {
                let cache = session.shared.lock();
                cache
                    .store
                    .path()
                    .filter(|_| cache.store.count() == target.blocks())
                    .map(PathBuf::from)
                    .ok_or_else(|| AppError::Path("the clip is still arriving".into()))?
            }
        };
        Ok((path, target.audio_tracks.clone()))
    }

    pub fn release_owner(&self, owner: &str) -> usize {
        let mut sessions = self.lock();
        let before = sessions.len();
        sessions.retain(|_, session| session.owner != owner);
        before - sessions.len()
    }

    pub fn clear(&self) {
        self.lock().clear();
    }

    /// Serves one range. Must be called off the async runtime.
    pub fn read(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Handle,
        node: Arc<Node>,
        stream_id: &str,
        owner: &str,
        range_header: Option<&str>,
    ) -> AppResult<RemoteVideoChunk> {
        let fetch = {
            let mut sessions = self.lock();
            let session = sessions
                .get_mut(stream_id)
                .filter(|session| session.owner == owner)
                .filter(|session| session.touched.elapsed() < SESSION_TTL)
                .ok_or_else(|| AppError::Path("stream session is unavailable".into()))?;
            session.touched = Instant::now();
            Fetch {
                runtime: runtime.clone(),
                node,
                target: session.target.clone(),
                shared: session.shared.clone(),
                progress: Arc::downgrade(self),
            }
        };
        let requested = resolve_range(range_header, fetch.target.size)
            .map_err(|error| AppError::Path(format!("invalid media byte range: {error:?}")))?;
        let first = requested.start / BLOCK_BYTES;
        runtime.block_on(async {
            let local = local_blocks(&fetch.target);
            if first < local {
                return read_local_range(&fetch.target, requested, local).await;
            }
            wait_for(&fetch, first, local).await?;
            Ok(serve(&fetch.shared, requested, first))
        })
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, HashMap<String, Session>> {
        self.sessions
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

/// Contiguous arrived bytes from `requested.start`, at most
/// `MAX_RESPONSE_BLOCKS` blocks.
fn serve(shared: &Shared, requested: ByteRange, first: u64) -> RemoteVideoChunk {
    let cache = shared.lock();
    let last = (requested.end_inclusive / BLOCK_BYTES).min(first + MAX_RESPONSE_BLOCKS - 1);
    let through = (first..=last)
        .take_while(|block| cache.store.contains(*block))
        .last()
        .unwrap_or(first);
    let end_exclusive = ((through + 1) * BLOCK_BYTES).min(requested.end_inclusive + 1);
    // Ranges never pass the clip's end, so the short last block fits; a
    // failed read serves nothing and the protocol answers with an error.
    let length = end_exclusive.saturating_sub(requested.start) as usize;
    let bytes = cache.store.read(requested.start, length).unwrap_or_default();
    RemoteVideoChunk {
        range: ByteRange {
            end_inclusive: requested.start + (bytes.len() as u64).max(1) - 1,
            ..requested
        },
        bytes,
    }
}

/// Whole-clip copies are deleted with their session, but a crash or an
/// open file handle can leave one behind. Only stale files go: a copy still
/// being written belongs to a live session elsewhere (another registry in
/// the same process, as in tests).
fn remove_leftover_copies() {
    const STALE: std::time::Duration = std::time::Duration::from_secs(10 * 60);
    let Ok(entries) = std::fs::read_dir(std::env::temp_dir()) else {
        return;
    };
    for entry in entries.flatten() {
        let stale = entry
            .metadata()
            .and_then(|metadata| metadata.modified())
            .is_ok_and(|modified| modified.elapsed().is_ok_and(|age| age > STALE));
        if stale && entry.file_name().to_string_lossy().starts_with(TEMP_PREFIX) {
            let _ = std::fs::remove_file(entry.path());
        }
    }
}

/// How many leading blocks a local copy (kept clip or partial) holds. A
/// metadata lookup, cheap enough to make from anywhere.
fn local_blocks(target: &StreamTarget) -> u64 {
    let mut best = 0;
    for path in &target.local {
        if let Ok(metadata) = std::fs::metadata(path) {
            let len = metadata.len();
            let blocks = if len >= target.size {
                target.blocks()
            } else {
                len / BLOCK_BYTES
            };
            best = best.max(blocks);
        }
    }
    best
}

/// Serves a range from a local copy, up to the end of its complete blocks.
async fn read_local_range(
    target: &StreamTarget,
    requested: ByteRange,
    local_blocks: u64,
) -> AppResult<RemoteVideoChunk> {
    let limit = (local_blocks * BLOCK_BYTES).min(target.size);
    let end_inclusive = requested
        .end_inclusive
        .min(limit - 1)
        .min(requested.start + MAX_RESPONSE_BLOCKS * BLOCK_BYTES - 1);
    let length = end_inclusive + 1 - requested.start;
    for path in &target.local {
        if let Some(bytes) = read_local(path, requested.start, length).await {
            return Ok(RemoteVideoChunk {
                range: ByteRange {
                    end_inclusive,
                    ..requested
                },
                bytes,
            });
        }
    }
    Err(AppError::Path(
        "the local copy of this clip is unavailable".into(),
    ))
}

/// The bytes from a local copy, if that copy already reaches past them.
async fn read_local(path: &PathBuf, start: u64, length: u64) -> Option<Vec<u8>> {
    let mut file = tokio::fs::File::open(path).await.ok()?;
    if file.metadata().await.ok()?.len() < start + length {
        return None;
    }
    file.seek(SeekFrom::Start(start)).await.ok()?;
    let mut bytes = vec![0_u8; length as usize];
    file.read_exact(&mut bytes).await.ok()?;
    Some(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn local_copies_serve_only_bytes_they_hold() {
        let folder = tempfile::tempdir().unwrap();
        let path = folder.path().join("partial");
        std::fs::write(&path, (0..=255_u8).cycle().take(1500).collect::<Vec<_>>()).unwrap();
        assert_eq!(read_local(&path, 1000, 500).await.unwrap()[0], (1000 % 256) as u8);
        assert!(read_local(&path, 1000, 501).await.is_none());
        assert!(read_local(&folder.path().join("missing"), 0, 1).await.is_none());
    }
}
