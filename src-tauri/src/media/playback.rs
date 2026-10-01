use std::{
    ffi::OsString,
    fs,
    path::{Path, PathBuf},
    sync::{Arc, OnceLock},
    time::Duration,
};

use crate::{
    clips::PathAuthorizer,
    error::{AppError, AppResult},
};

use super::{
    audio_edit_list_patches, library_input, resolve_range, ByteRange, FfmpegExecutor, FfmpegJob,
    MediaSessionRegistry, PlaybackDescriptor, PlaybackPatch, RemoteMediaSource, RemoteVideoChunk,
    ThumbnailService,
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
    remote: OnceLock<Arc<dyn RemoteMediaSource>>,
}

impl MediaService {
    pub fn new(sessions: Arc<MediaSessionRegistry>, ffmpeg: Arc<dyn FfmpegExecutor>) -> Self {
        Self {
            sessions,
            thumbnails: ThumbnailService::new(ffmpeg.clone()),
            ffmpeg,
            remote: OnceLock::new(),
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
        self.mix_audio(
            &session.path,
            &session.selected_audio_indexes,
            start_seconds,
            duration_seconds,
        )
    }

    /// Every audio track of a friend's clip, mixed, once it has fully
    /// arrived; a video element alone plays only the first track.
    pub fn remote_mixed_audio_chunk(
        &self,
        stream_id: &str,
        owner: &str,
        start_seconds: f64,
        duration_seconds: f64,
    ) -> AppResult<Vec<u8>> {
        let file = self
            .remote
            .get()
            .ok_or_else(|| AppError::Path("remote media is unavailable".into()))?
            .complete_file(stream_id, owner)?;
        let indexes = super::sessions::selected_audio_indexes(&file.audio_tracks);
        self.mix_audio(&file.path, &indexes, start_seconds, duration_seconds)
    }

    fn mix_audio(
        &self,
        path: &std::path::Path,
        indexes: &[u8],
        start_seconds: f64,
        duration_seconds: f64,
    ) -> AppResult<Vec<u8>> {
        if indexes.is_empty() {
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
        let filter = mixed_audio_filter(indexes);
        let head = [
            OsString::from("-nostdin"),
            OsString::from("-hide_banner"),
            OsString::from("-loglevel"),
            OsString::from("error"),
        ];
        let mut job = FfmpegJob::new(
            "mixed audio preview",
            head.into_iter().chain(library_input(path)).chain([
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
            ]),
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

    /// A copy of a clip laid out for streaming to a friend; `false` when
    /// the clip already streams well and no copy was written.
    pub fn write_stream_copy(&self, input: &Path, output: &Path) -> AppResult<bool> {
        super::layout_repair::write_stream_copy(self.ffmpeg.as_ref(), input, output)
    }

    pub fn thumbnail(&self, authority: &PathAuthorizer, clip_id: &str) -> AppResult<String> {
        self.thumbnails.thumbnail(authority, clip_id)
    }

    pub fn release_session(&self, session_id: &str, owner: &str) -> bool {
        self.sessions.release(session_id, owner)
    }

    /// Player switch/close: playback sessions only. Thumbnails stay cached so
    /// the library does not re-run FFmpeg for every card after each clip.
    pub fn release_owner(&self, owner: &str) -> usize {
        self.sessions.release_owner(owner) + self.release_remote(owner)
    }

    /// The UI owner is gone (window destroyed or UI process exited).
    pub fn release_ui(&self, owner: &str) -> usize {
        self.thumbnails.clear();
        self.sessions.release_owner(owner) + self.release_remote(owner)
    }

    /// Installs the source for `/v1/remote/<id>/video` once during setup.
    pub fn set_remote_source(&self, source: Arc<dyn RemoteMediaSource>) {
        let _ = self.remote.set(source);
    }

    pub fn remote_url(&self, stream_id: &str) -> String {
        format!("{}/v1/remote/{stream_id}/video", self.sessions.endpoint())
    }


    pub fn remote_video(
        &self,
        stream_id: &str,
        owner: &str,
        range_header: Option<&str>,
    ) -> AppResult<RemoteVideoChunk> {
        self.remote
            .get()
            .ok_or_else(|| AppError::Path("remote media is unavailable".into()))?
            .read_video(stream_id, owner, range_header)
    }

    fn release_remote(&self, owner: &str) -> usize {
        self.remote
            .get()
            .map_or(0, |remote| remote.release_owner(owner))
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

    use crate::{contracts::ClipRecord, media::FfmpegOutput};
    use std::sync::atomic::{AtomicUsize, Ordering};

    struct CountingFfmpeg(AtomicUsize);
    impl FfmpegExecutor for CountingFfmpeg {
        fn run(&self, _: FfmpegJob) -> AppResult<FfmpegOutput> {
            self.0.fetch_add(1, Ordering::Relaxed);
            Ok(FfmpegOutput {
                success: true,
                exit_code: Some(0),
                stdout: vec![0xff, 0xd8, 0xff],
                stderr: String::new(),
            })
        }
    }

    #[test]
    fn switching_clips_keeps_thumbnails_but_ui_teardown_clears_them() {
        let root = tempfile::tempdir().unwrap();
        let video = root.path().join("clip.mp4");
        fs::write(&video, b"video").unwrap();
        let authority = PathAuthorizer::from_records([ClipRecord {
            id: "clip".into(),
            file_path: video.to_string_lossy().into(),
            ..ClipRecord::default()
        }]);
        let ffmpeg = Arc::new(CountingFfmpeg(AtomicUsize::new(0)));
        let sessions = Arc::new(MediaSessionRegistry::new("clipture-media://localhost").unwrap());
        let media = MediaService::new(sessions, ffmpeg.clone());
        media.thumbnail(&authority, "clip").unwrap();
        media.release_owner("main");
        media.thumbnail(&authority, "clip").unwrap();
        assert_eq!(ffmpeg.0.load(Ordering::Relaxed), 1, "player release must not evict thumbnails");
        media.release_ui("main");
        media.thumbnail(&authority, "clip").unwrap();
        assert_eq!(ffmpeg.0.load(Ordering::Relaxed), 2, "UI teardown frees the thumbnail cache");
    }

    #[test]
    fn mix_filter_aligns_sparse_tracks_before_mix() {
        let filter = mixed_audio_filter(&[0, 2]);
        assert!(filter.contains("[0:a:0]aresample"));
        assert!(filter.contains("[0:a:2]aresample"));
        assert!(filter.contains("amix=inputs=2"));
    }
}
