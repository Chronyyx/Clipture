//! "Add to library": verified copies of a friend's clip, run by the resident
//! controller so they finish even if the UI closes.
use std::{path::PathBuf, sync::Arc, time::Instant};

use crate::{
    contracts::ClipRecord,
    error::{AppError, AppResult},
    library::iso_utc,
    media::audio_track_count,
};

use super::super::{
    download::{download, partial_path, Progress},
    model::{ClipOffer, Download, DownloadPhase},
    node::parse_friend_code,
};
use super::{SharingService, MAX_DOWNLOAD_ROWS, PROGRESS_INTERVAL};

impl SharingService {
    /// Starts a verified download into the library; progress is reported
    /// through snapshots. Returns immediately.
    pub fn save_shared_clip(self: &Arc<Self>, share_id: &str) -> AppResult<()> {
        let node = self.require_node()?;
        let clip = self
            .core
            .read(|state| {
                state
                    .inbox_clip(share_id)
                    .filter(|clip| state.is_accepted(&clip.friend_id) && !clip.awaiting_answer)
                    .cloned()
            })
            .ok_or_else(|| AppError::Path("this shared clip is no longer available".into()))?;
        if clip
            .saved_path
            .as_deref()
            .is_some_and(|path| std::path::Path::new(path).is_file())
        {
            return Ok(());
        }
        {
            let mut downloads = self.lock_downloads();
            if downloads
                .iter()
                .any(|row| row.share_id == share_id && row.phase == DownloadPhase::Running)
            {
                return Ok(());
            }
            downloads.retain(|row| row.share_id != share_id);
            if downloads.len() >= MAX_DOWNLOAD_ROWS {
                if let Some(index) = downloads
                    .iter()
                    .position(|row| row.phase != DownloadPhase::Running)
                {
                    downloads.remove(index);
                } else {
                    return Err(AppError::Path("too many downloads are running".into()));
                }
            }
            downloads.push(Download {
                share_id: share_id.into(),
                received_bytes: 0,
                total_bytes: clip.offer.size,
                phase: DownloadPhase::Running,
                message: None,
                bytes_per_second: 0,
                relayed: None,
                reconnecting: false,
                abort: None,
            });
        }
        self.core.events.changed();
        let peer = parse_friend_code(&clip.friend_id)?;
        let friend_name = self.core.read(|state| {
            state
                .friend(&clip.friend_id)
                .map(|friend| friend.name.clone())
                .unwrap_or_default()
        });
        let this = self.clone();
        let task = self.runtime.spawn(async move {
            let folder = this.library.shared_clips_folder();
            let mut last_emit = Instant::now();
            // Unknown until the first report, which counts bytes resumed
            // from disk rather than received over the network.
            let mut last_bytes: Option<u64> = None;
            let result = download(&node, peer, &clip.offer, &folder, |progress| {
                let elapsed = last_emit.elapsed();
                let first = last_bytes.is_none();
                let speed = if progress.reconnecting {
                    Some(0)
                } else {
                    last_bytes.filter(|_| elapsed >= PROGRESS_INTERVAL).map(|last| {
                        let bytes = progress.received.saturating_sub(last);
                        (bytes as f64 / elapsed.as_secs_f64()) as u64
                    })
                };
                this.set_progress(&clip.offer.share_id, &progress, speed);
                if first || speed.is_some() {
                    last_emit = Instant::now();
                    last_bytes = Some(progress.received);
                    this.core.events.changed();
                }
            })
            .await
            .and_then(|path| this.publish_download(&clip.offer, &friend_name, path));
            if result.is_ok() {
                this.confirm_kept(&clip.offer.share_id);
            }
            this.finish_download(
                &clip.offer.share_id,
                result.err().map(|error| error.to_string()),
            );
        });
        if let Some(row) = self
            .lock_downloads()
            .iter_mut()
            .find(|row| row.share_id == share_id && row.phase == DownloadPhase::Running)
        {
            row.abort = Some(task.abort_handle());
        }
        Ok(())
    }

    /// Tells the sender a verified copy is in the library, so they stop
    /// serving it: the friend has it now.
    fn confirm_kept(self: &Arc<Self>, share_id: &str) {
        if let Ok(Some(friend_id)) = self.core.update(|state| state.confirm_kept(share_id)) {
            self.deliver_to_soon(friend_id);
        }
    }

    /// Stops an "add to library" transfer and deletes what was received;
    /// nothing reaches the library.
    pub fn cancel_download(&self, share_id: &str) {
        let removed = {
            let mut downloads = self.lock_downloads();
            let index = downloads.iter().position(|row| row.share_id == share_id);
            index.map(|index| downloads.remove(index))
        };
        if let Some(row) = removed {
            if let Some(abort) = row.abort {
                abort.abort();
            }
            self.core.events.changed();
        }
        self.discard_partial(share_id);
    }

    /// Deletes a share's unfinished download. The aborted task may still
    /// hold the file for a moment, and Windows cannot delete an open file.
    pub(super) fn discard_partial(&self, share_id: &str) {
        let path = partial_path(&self.library.shared_clips_folder(), share_id);
        self.runtime.spawn(async move {
            for _ in 0..20 {
                match tokio::fs::remove_file(&path).await {
                    Err(error) if error.kind() != std::io::ErrorKind::NotFound => {
                        tokio::time::sleep(std::time::Duration::from_millis(100)).await;
                    }
                    _ => return,
                }
            }
        });
    }

    pub(super) fn publish_download(
        &self,
        offer: &ClipOffer,
        friend_name: &str,
        path: PathBuf,
    ) -> AppResult<()> {
        let file_path = path.to_string_lossy().into_owned();
        self.library.publish(ClipRecord {
            id: format!("shared-{}", offer.share_id),
            title: offer.title.clone(),
            game_or_app: if offer.game_or_app.is_empty() {
                "Shared".into()
            } else {
                offer.game_or_app.clone()
            },
            folder_name: Some(if friend_name.is_empty() {
                "Shared with me".into()
            } else {
                format!("From {friend_name}")
            }),
            created_at: iso_utc(std::time::SystemTime::now()),
            duration_seconds: offer.duration_seconds,
            file_path: file_path.clone(),
            resolution: offer.resolution.clone(),
            fps: offer.fps,
            encoder: "shared".into(),
            audio_tracks: track_labels(&offer.audio_tracks, audio_track_count(&path)),
            ..ClipRecord::default()
        })?;
        self.core.update(|state| {
            if let Some(clip) = state
                .inbox
                .iter_mut()
                .find(|clip| clip.offer.share_id == offer.share_id)
            {
                clip.saved_path = Some(file_path);
            }
        })?;
        self.core.events.library_changed();
        Ok(())
    }

    pub(super) fn set_progress(&self, share_id: &str, progress: &Progress, speed: Option<u64>) {
        if let Some(row) = self
            .lock_downloads()
            .iter_mut()
            .find(|row| row.share_id == share_id)
        {
            row.received_bytes = progress.received;
            row.relayed = progress.relayed;
            row.reconnecting = progress.reconnecting;
            if let Some(speed) = speed {
                row.bytes_per_second = speed;
            }
        }
    }

    pub(super) fn finish_download(&self, share_id: &str, error: Option<String>) {
        // What a retry would resume from; nothing once the partial is gone.
        let kept = error.is_some()
            && partial_path(&self.library.shared_clips_folder(), share_id).is_file();
        if let Some(row) = self
            .lock_downloads()
            .iter_mut()
            .find(|row| row.share_id == share_id)
        {
            row.phase = if error.is_some() {
                DownloadPhase::Failed
            } else {
                DownloadPhase::Done
            };
            if error.is_none() {
                row.received_bytes = row.total_bytes;
            } else if !kept {
                row.received_bytes = 0;
            }
            row.bytes_per_second = 0;
            row.reconnecting = false;
            row.message = error;
        }
        self.core.events.changed();
    }

    pub(super) fn lock_downloads(&self) -> std::sync::MutexGuard<'_, Vec<Download>> {
        self.downloads
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

/// One label per audio track actually in the file: the sender's labels where
/// they exist, numbered ones for any extra tracks. The library mixes tracks
/// by this list, so a short list would silence the rest.
fn track_labels(offered: &[String], in_file: Option<usize>) -> Vec<String> {
    let Some(count) = in_file else {
        return offered.to_vec();
    };
    (0..count)
        .map(|index| {
            offered
                .get(index)
                .cloned()
                .unwrap_or_else(|| format!("Track {}", index + 1))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::track_labels;

    #[test]
    fn labels_follow_the_tracks_in_the_file() {
        let offered = vec!["System audio".to_owned()];
        assert_eq!(
            track_labels(&offered, Some(3)),
            ["System audio", "Track 2", "Track 3"]
        );
        assert_eq!(track_labels(&offered, Some(0)), Vec::<String>::new());
        assert_eq!(track_labels(&offered, None), offered);
    }
}
