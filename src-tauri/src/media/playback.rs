use std::{ffi::OsString, fs, path::PathBuf, sync::Arc, time::Duration};

use crate::{
    clips::PathAuthorizer,
    error::{AppError, AppResult},
};

use super::{
    audio_edit_list_patches, resolve_range, ByteRange, FfmpegExecutor, FfmpegJob,
    MediaSessionRegistry, PlaybackDescriptor, PlaybackPatch, ThumbnailService,
};

const MAXIMUM_AUDIO_CHUNK_BYTES: usize = 8 * 1024 * 1024;

#[derive(Clone, Debug)]
pub struct VideoStreamPlan {
    pub path: PathBuf,
    pub content_type: &'static str,
    pub range: ByteRange,
    pub patches: Vec<PlaybackPatch>,
}

pub struct MediaService {
    sessions: Arc<MediaSessionRegistry>,
    ffmpeg: Arc<dyn FfmpegExecutor>,
    thumbnails: ThumbnailService,
}

impl MediaService {
    pub fn new(sessions: Arc<MediaSessionRegistry>, ffmpeg: Arc<dyn FfmpegExecutor>) -> Self {
        Self {
            sessions,
            thumbnails: ThumbnailService::new(ffmpeg.clone()),
            ffmpeg,
        }
    }

    pub fn open_playback(
        &self,
        authority: &PathAuthorizer,
        clip_id: &str,
        audio_tracks: &[String],
        owner: &str,
    ) -> AppResult<PlaybackDescriptor> {
        let path = authority
            .primary(clip_id)
            .ok_or_else(|| AppError::Path("clip is not authorized for playback".into()))?;
        let patches = audio_edit_list_patches(path);
        self.sessions
            .open(authority, clip_id, audio_tracks, patches, owner)
    }

    pub fn video_stream_plan(
        &self,
        session_id: &str,
        owner: &str,
        range_header: Option<&str>,
    ) -> AppResult<VideoStreamPlan> {
        let session = self.sessions.resolve(session_id, owner)?;
        let total = fs::metadata(&session.path)
            .map_err(|source| AppError::Io {
                action: "inspect playback media",
                path: session.path.clone(),
                source,
            })?
            .len();
        let range = resolve_range(range_header, total)
            .map_err(|error| AppError::Path(format!("invalid media byte range: {error:?}")))?;
        Ok(VideoStreamPlan {
            path: session.path,
            content_type: session.content_type,
            range,
            patches: session.playback_patches,
        })
    }

    pub fn mixed_audio_chunk(
        &self,
        session_id: &str,
        owner: &str,
        start_seconds: f64,
        duration_seconds: f64,
    ) -> AppResult<Vec<u8>> {
        let session = self.sessions.resolve(session_id, owner)?;
        if session.selected_audio_indexes.is_empty() {
            return Err(AppError::Path(
                "playback session has no selected audio tracks".into(),
            ));
        }
        let start = if start_seconds.is_finite() {
            start_seconds.max(0.0)
        } else {
            0.0
        };
        let duration = if duration_seconds.is_finite() {
            duration_seconds.clamp(0.25, 20.0)
        } else {
            8.0
        };
        let filter = mixed_audio_filter(&session.selected_audio_indexes);
        let mut job = FfmpegJob::new(
            "mixed audio preview",
            [
                OsString::from("-nostdin"),
                OsString::from("-hide_banner"),
                OsString::from("-loglevel"),
                OsString::from("error"),
                OsString::from("-i"),
                session.path.as_os_str().to_owned(),
                OsString::from("-filter_complex"),
                OsString::from(filter),
                OsString::from("-map"),
                OsString::from("[aout]"),
                // Output-side seeking preserves MP4 edit-list gaps.
                OsString::from("-ss"),
                OsString::from(format!("{start:.6}")),
                OsString::from("-t"),
                OsString::from(format!("{duration:.6}")),
                OsString::from("-vn"),
                OsString::from("-sn"),
                OsString::from("-dn"),
                OsString::from("-ac"),
                OsString::from("2"),
                OsString::from("-ar"),
                OsString::from("48000"),
                OsString::from("-f"),
                OsString::from("wav"),
                OsString::from("pipe:1"),
            ],
        );
        job.timeout = Duration::from_secs(25);
        job.maximum_stdout_bytes = MAXIMUM_AUDIO_CHUNK_BYTES;
        let output = self.ffmpeg.run(job)?;
        if output.success && !output.stdout.is_empty() {
            Ok(output.stdout)
        } else {
            Err(AppError::Integration(if output.stderr.is_empty() {
                format!("mixed audio FFmpeg exited with code {:?}", output.exit_code)
            } else {
                output.stderr
            }))
        }
    }

    pub fn thumbnail(&self, authority: &PathAuthorizer, clip_id: &str) -> AppResult<String> {
        self.thumbnails.thumbnail(authority, clip_id)
    }

    pub fn release_session(&self, session_id: &str, owner: &str) -> bool {
        self.sessions.release(session_id, owner)
    }

    pub fn release_owner(&self, owner: &str) -> usize {
        self.thumbnails.clear();
        self.sessions.release_owner(owner)
    }
}

fn mixed_audio_filter(indexes: &[u8]) -> String {
    if indexes.len() == 1 {
        return format!(
            "[0:a:{}]aresample=48000:async=1:first_pts=0,apad[aout]",
            indexes[0]
        );
    }
    let aligned: Vec<_> = indexes
        .iter()
        .enumerate()
        .map(|(position, index)| {
            format!("[0:a:{index}]aresample=48000:async=1:first_pts=0,apad[aligned{position}]")
        })
        .collect();
    let inputs: String = (0..indexes.len())
        .map(|position| format!("[aligned{position}]"))
        .collect();
    format!(
        "{};{}amix=inputs={}:duration=longest:dropout_transition=0[aout]",
        aligned.join(";"),
        inputs,
        indexes.len()
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mix_filter_aligns_sparse_tracks_before_mix() {
        let filter = mixed_audio_filter(&[0, 2]);
        assert!(filter.contains("[0:a:0]aresample"));
        assert!(filter.contains("[0:a:2]aresample"));
        assert!(filter.contains("amix=inputs=2"));
    }
}
