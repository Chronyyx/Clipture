use std::{
    ffi::OsString,
    fs,
    path::PathBuf,
    sync::{Arc, OnceLock},
    time::Duration,
};

use crate::{
    clips::PathAuthorizer,
    error::{AppError, AppResult},
};

use super::{
    audio_edit_list_patches, library_input, resolve_range, ByteRange, ClipMixer, ClipTrimmer, FfmpegExecutor,
    FfmpegJob, MixError, MixedClip, TrimError, TrimmedClip, WaveformBuilder,
    LinearView, MediaSessionRegistry, PlaybackDescriptor, PlaybackPatch, RemoteMediaSource,
    RemoteVideoChunk, ThumbnailService,
};

const MAXIMUM_AUDIO_CHUNK_BYTES: usize = 8 * 1024 * 1024;
const MAXIMUM_SPLIT_TRACKS: usize = 16;

#[derive(Clone, Debug)]
pub struct VideoStreamPlan {
    pub path: PathBuf,
    pub content_type: &'static str,
    pub range: ByteRange,
    pub patches: Vec<PlaybackPatch>,
    /// Read through this instead of `path` (patches already applied).
    pub view: Option<Arc<LinearView>>,
}

pub struct MediaService {
    sessions: Arc<MediaSessionRegistry>,
    ffmpeg: Arc<dyn FfmpegExecutor>,
    thumbnails: ThumbnailService,
    remote: OnceLock<Arc<dyn RemoteMediaSource>>,
    trimmer: Option<ClipTrimmer>,
    mixer: Option<ClipMixer>,
    waveforms: Option<WaveformBuilder>,
}

impl MediaService {
    pub fn new(sessions: Arc<MediaSessionRegistry>, ffmpeg: Arc<dyn FfmpegExecutor>) -> Self {
        Self {
            sessions,
            thumbnails: ThumbnailService::new(ffmpeg.clone()),
            ffmpeg,
            remote: OnceLock::new(),
            trimmer: None,
            mixer: None,
            waveforms: None,
        }
    }

    /// Builds clip waveforms with `ffmpeg` (its own slots, so a waveform
    /// never holds up playback audio), using `scratch` while it works.
    pub fn with_waveforms(mut self, scratch: PathBuf, ffmpeg: Arc<dyn FfmpegExecutor>) -> Self {
        self.waveforms = Some(WaveformBuilder::new(scratch, ffmpeg));
        self
    }

    /// Enables baking audio levels, with its crash journals kept in `journals`.
    pub fn with_mix_journals(mut self, journals: PathBuf) -> Self {
        self.mixer = Some(ClipMixer::new(journals, self.ffmpeg.clone()));
        self
    }

    /// Completes audio mixes a crash interrupted. Call before clips are read.
    pub fn finish_interrupted_mixes(&self) -> Vec<MixedClip> {
        self.mixer.as_ref().map(ClipMixer::finish_interrupted).unwrap_or_default()
    }

    /// Re-encodes the clip's k-th audio track at `gains[k]` (0 silences it)
    /// for good. Players of the clip are stopped first. Blocking.
    pub fn mix(&self, path: &std::path::Path, gains: &[f64]) -> Result<MixedClip, MixError> {
        let mixer = self.mixer.as_ref().ok_or(MixError::Unsupported("audio levels cannot be changed here"))?;
        self.sessions.release_path(path);
        mixer.mix(path, gains)
    }

    /// Enables trimming, with its crash journals kept in `journals`.
    pub fn with_trim_journals(mut self, journals: PathBuf) -> Self {
        self.trimmer = Some(ClipTrimmer::new(journals));
        self
    }

    /// Completes trims a crash interrupted. Call before clips are read.
    pub fn finish_interrupted_trims(&self) -> Vec<TrimmedClip> {
        self.trimmer.as_ref().map(ClipTrimmer::finish_interrupted).unwrap_or_default()
    }

    /// Keeps `[start, end)` seconds of the clip at `path` and deletes the
    /// rest of it from the file. Players of the clip are stopped first.
    /// Blocking.
    pub fn trim(&self, path: &std::path::Path, start: f64, end: f64) -> Result<TrimmedClip, TrimError> {
        let trimmer = self.trimmer.as_ref().ok_or(TrimError::Unsupported("trimming is not available"))?;
        self.sessions.release_path(path);
        let result = trimmer.trim(path, start, end);
        self.thumbnails.invalidate(path);
        result
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
        // Save in place stores frames wherever the replay buffer had room;
        // played as stored, the player jumps across the file and stalls.
        let view = match LinearView::open(path, &patches) {
            Ok(view) => view.map(Arc::new),
            Err(error) => {
                tracing::warn!(%error, "could not lay the clip out for playback; playing it as stored");
                None
            }
        };
        self.sessions
            .open(authority, clip_id, audio_tracks, patches, view, owner)
    }

    pub fn video_stream_plan(
        &self,
        session_id: &str,
        owner: &str,
        range_header: Option<&str>,
    ) -> AppResult<VideoStreamPlan> {
        let session = self.sessions.resolve(session_id, owner)?;
        let total = match &session.view {
            Some(view) if !view.is_current() => {
                return Err(AppError::Path("the clip changed while it was open".into()))
            }
            Some(view) => view.len(),
            None => fs::metadata(&session.path)
                .map_err(|source| AppError::Io {
                    action: "inspect playback media",
                    path: session.path.clone(),
                    source,
                })?
                .len(),
        };
        let range = resolve_range(range_header, total)
            .map_err(|error| AppError::Path(format!("invalid media byte range: {error:?}")))?;
        Ok(VideoStreamPlan {
            path: session.path,
            content_type: session.content_type,
            range,
            patches: if session.view.is_some() { Vec::new() } else { session.playback_patches },
            view: session.view,
        })
    }

    /// The session's audio, mixed; or with `tracks`, each track as its own
    /// stereo pair (in `audio_track_indexes` order) for a live mixer.
    pub fn mixed_audio_chunk(
        &self,
        session_id: &str,
        owner: &str,
        start_seconds: f64,
        duration_seconds: f64,
        tracks: bool,
    ) -> AppResult<Vec<u8>> {
        let session = self.sessions.resolve(session_id, owner)?;
        if tracks && session.selected_audio_indexes.len() > MAXIMUM_SPLIT_TRACKS {
            return Err(AppError::Path("too many audio tracks to play apart".into()));
        }
        let longest = super::sessions::track_chunk_seconds(session.selected_audio_indexes.len());
        if tracks && duration_seconds > f64::from(longest) + 0.001 {
            return Err(AppError::Path("audio chunk is too long to hold its tracks apart".into()));
        }
        self.audio_chunk(
            &session.path,
            &session.selected_audio_indexes,
            start_seconds,
            duration_seconds,
            tracks,
        )
    }

    /// The session's played tracks, mixed, as an audiowaveform JSON reply.
    pub fn waveform(&self, session_id: &str, owner: &str) -> AppResult<Vec<u8>> {
        let session = self.sessions.resolve(session_id, owner)?;
        self.waveforms
            .as_ref()
            .ok_or_else(|| AppError::Path("waveforms are not available".into()))?
            .build(&session.path, &session.selected_audio_indexes)
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
        self.audio_chunk(&file.path, &indexes, start_seconds, duration_seconds, false)
    }

    fn audio_chunk(
        &self,
        path: &std::path::Path,
        indexes: &[u8],
        start_seconds: f64,
        duration_seconds: f64,
        tracks: bool,
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
        let filter = if tracks { track_audio_filter(indexes) } else { mixed_audio_filter(indexes) };
        // Stereo out of the mix; with tracks apart, two channels per track.
        let channels = if tracks { indexes.len() * 2 } else { 2 };
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
                OsString::from(channels.to_string()),
                OsString::from("-ar"),
                OsString::from("48000"),
                OsString::from("-f"),
                OsString::from("wav"),
                OsString::from("pipe:1"),
            ]),
        );
        job.timeout = Duration::from_secs(25);
        job.maximum_stdout_bytes = if tracks { super::MAXIMUM_TRACK_CHUNK_BYTES + 4096 } else { MAXIMUM_AUDIO_CHUNK_BYTES };
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

/// Each track aligned like the mix, as stereo, side by side in one stream.
fn track_audio_filter(indexes: &[u8]) -> String {
    let aligned: Vec<_> = indexes
        .iter()
        .enumerate()
        .map(|(position, index)| {
            let label = if indexes.len() == 1 { "aout".to_owned() } else { format!("track{position}") };
            format!("[0:a:{index}]aresample=48000:async=1:first_pts=0,aformat=channel_layouts=stereo,apad[{label}]")
        })
        .collect();
    if indexes.len() == 1 {
        return aligned.join(";");
    }
    let inputs: String = (0..indexes.len()).map(|position| format!("[track{position}]")).collect();
    format!("{};{}amerge=inputs={}[aout]", aligned.join(";"), inputs, indexes.len())
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

    #[test]
    fn chunks_with_tracks_apart_fit_their_byte_budget() {
        for tracks in 1..=MAXIMUM_SPLIT_TRACKS {
            let seconds = super::super::sessions::track_chunk_seconds(tracks) as usize;
            assert!(seconds >= 1);
            assert!(seconds * tracks * 4 * 48_000 <= super::super::MAXIMUM_TRACK_CHUNK_BYTES, "{tracks} tracks");
        }
        assert_eq!(super::super::sessions::track_chunk_seconds(1), 8);
        assert_eq!(super::super::sessions::track_chunk_seconds(5), 3);
    }

    #[test]
    fn track_filter_keeps_each_track_apart_as_stereo() {
        let filter = track_audio_filter(&[0, 2]);
        assert!(filter.contains("[0:a:0]aresample=48000:async=1:first_pts=0,aformat=channel_layouts=stereo,apad[track0]"));
        assert!(filter.contains("[0:a:2]aresample"));
        assert!(filter.ends_with("[track0][track1]amerge=inputs=2[aout]"));
        assert!(track_audio_filter(&[1]).ends_with("apad[aout]"));
    }
}
