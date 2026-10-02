//! Offering clips to friends. A clip that streams badly as recorded (in-place
//! recordings can be part zero padding, with frames out of time order) is
//! sent through its linear view: the same samples in playing order, index
//! first, read from the clip as the friend asks for them, without writing a
//! copy. Builds before 1.6.5 sent a remuxed copy from
//! `<save folder>/.clipture-sharing` instead; those shares keep working, and
//! their copies are still deleted when the share ends, past a size cap or
//! an age limit.
use std::{path::PathBuf, sync::Arc, time::Duration};

use crate::{
    error::{AppError, AppResult},
    media::{random_token, LinearView},
};

use super::super::{
    core::now_ms,
    model::{ClipOffer, OutgoingShare, ShareAnswer},
    wire::{self, MAX_CLIP_BYTES},
};
use super::{hash_file, hash_view, ShareSource, SharingService};

/// Disk the stream copies may use together; the oldest shares go first.
const MAX_COPY_BYTES: u64 = 10 * 1024 * 1024 * 1024;
/// Copies older than this are dropped with their share.
const MAX_COPY_AGE: Duration = Duration::from_secs(30 * 24 * 60 * 60);
/// Unregistered copies younger than this may still be in the making.
const IN_PROGRESS: Duration = Duration::from_secs(10 * 60);

impl SharingService {
    /// Hashes the clip and offers it to one accepted friend. Blocking.
    pub fn share_clip(self: &Arc<Self>, friend_id: &str, source: ShareSource) -> AppResult<String> {
        if !self.core.read(|state| state.is_accepted(friend_id)) {
            return Err(AppError::Path(
                "you can only share with accepted friends".into(),
            ));
        }
        let (source_size, source_digest) = hash_file(&source.path)?;
        if source_size == 0 || source_size > MAX_CLIP_BYTES {
            return Err(AppError::Path("this clip is too large to share".into()));
        }
        // Sharing again re-sends: the friend may have removed it from their
        // list. Unchanged, the same share goes out again; edited since (or an
        // older build's copy is gone), the old share no longer matches and is
        // replaced.
        let existing = self.core.update_when(|state| {
            let index = state
                .outbox
                .iter()
                .position(|share| share.friend_id == friend_id && share.original() == source.path);
            let Some(index) = index else {
                return (None, false);
            };
            let share = &mut state.outbox[index];
            if share.source_digest() == source_digest && share.path.is_file() {
                // Asked again: they may decline this time, or accept a clip
                // they declined before.
                share.delivered = false;
                share.answer = ShareAnswer::Pending;
                share.received_whole = false;
                share.kept = false;
                share.accepted_at_ms = None;
                share.keep_started = false;
                (Some(share.offer.share_id.clone()), true)
            } else {
                state.outbox.remove(index);
                (None, true)
            }
        })?;
        if let Some(existing) = existing {
            self.transfers.forget(&existing);
            self.deliver_soon();
            return Ok(existing);
        }

        let share_id: String = random_token().chars().take(32).collect();
        let view = match LinearView::open(&source.path, &[]) {
            Ok(view) => view,
            Err(error) => {
                // Sending the clip as recorded still works, just slower.
                tracing::warn!(%error, "could not lay the clip out for sending; sharing it as stored");
                None
            }
        };
        let (size, digest) = match &view {
            Some(view) => hash_view(view)?,
            None => (source_size, source_digest.clone()),
        };
        let offer = ClipOffer {
            share_id: share_id.clone(),
            title: wire::clean_text(&source.title, 200),
            size,
            blake3: digest,
            duration_seconds: source.duration_seconds,
            resolution: source.resolution,
            game_or_app: source.game_or_app,
            created_at_ms: now_ms(),
            fps: source.fps,
            audio_tracks: source.audio_tracks,
        };
        let share = OutgoingShare {
            offer,
            friend_id: friend_id.into(),
            path: source.path,
            delivered: false,
            source: None,
            source_blake3: Some(source_digest),
            linear: view.is_some(),
            answer: ShareAnswer::Pending,
            received_whole: false,
            kept: false,
            accepted_at_ms: None,
            keep_started: false,
        };
        self.core.update(|state| state.add_outgoing(share))?;
        self.tidy_copies();
        self.deliver_soon();
        Ok(share_id)
    }

    /// Stops a friend from streaming or downloading a clip we shared.
    pub fn revoke_share(&self, share_id: &str) -> AppResult<()> {
        self.core.update(|state| {
            state
                .outbox
                .retain(|share| share.offer.share_id != share_id)
        })?;
        self.transfers.forget(share_id);
        self.tidy_copies();
        Ok(())
    }

    /// Ends shares whose copies are too old or over the size cap (oldest
    /// first), then deletes copies no share uses any more.
    pub(super) fn tidy_copies(&self) {
        let folder = self.library.outgoing_copies_folder();
        let now = now_ms();
        let _ = self.core.update_when(|state| {
            let before = state.outbox.len();
            let mut copies: Vec<(u64, String, u64)> = state
                .outbox
                .iter()
                .filter(|share| share.source.is_some())
                .map(|share| {
                    let bytes = std::fs::metadata(&share.path).map_or(0, |m| m.len());
                    (share.offer.created_at_ms, share.offer.share_id.clone(), bytes)
                })
                .collect();
            copies.sort_unstable();
            let mut total: u64 = copies.iter().map(|(_, _, bytes)| bytes).sum();
            let mut ended = Vec::new();
            for (created, share_id, bytes) in copies {
                let expired = now.saturating_sub(created) > MAX_COPY_AGE.as_millis() as u64;
                if expired || total > MAX_COPY_BYTES {
                    total -= bytes;
                    ended.push(share_id);
                }
            }
            state.outbox.retain(|share| !ended.contains(&share.offer.share_id));
            ((), state.outbox.len() != before)
        });
        let used: Vec<PathBuf> = self.core.read(|state| {
            state
                .outbox
                .iter()
                // A kept share is never served again, so its copy can go.
                .filter(|share| share.source.is_some() && !share.kept)
                .map(|share| share.path.clone())
                .collect()
        });
        let Ok(entries) = std::fs::read_dir(&folder) else {
            return;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let ours = path.extension().is_some_and(|extension| extension == "mp4");
            // A copy another share is still making is not registered yet.
            let settled = entry
                .metadata()
                .and_then(|metadata| metadata.modified())
                .is_ok_and(|modified| modified.elapsed().is_ok_and(|age| age > IN_PROGRESS));
            if ours && settled && !used.contains(&path) {
                let _ = std::fs::remove_file(&path);
            }
        }
    }
}
