//! An overview of a clip's audio for the trim editor, detailed enough to read
//! like an editor's waveform: the lowest and highest sample of every 10 ms
//! of the clip (every played track mixed), so loud moments (a shot, a shout,
//! a goal) and quiet ones can be found by eye.
//!
//! Built when asked and not kept: the clip's audio packets alone are read
//! with its index (a few percent of the file), each played track is decoded
//! to low-rate mono by its own FFmpeg in parallel, and the tracks are summed
//! here. The reply is in audiowaveform's JSON format, which the trim
//! editor's views draw.
use std::{
    ffi::OsString,
    fs,
    path::{Path, PathBuf},
    sync::Arc,
    time::Duration,
};

use serde::Serialize;

use crate::error::{AppError, AppResult};

use super::{audio_extract::extract_audio, library_input, random_token, FfmpegExecutor, FfmpegJob};

/// Decoded at this rate: transients survive, long clips stay small.
const RATE: u32 = 8_000;
/// One min/max pair per 10 ms; long clips get coarser pairs instead.
const SAMPLES_PER_PIXEL: usize = (RATE / 100) as usize;
const MAXIMUM_PAIRS: usize = 40_000;
/// 16-bit mono at `RATE`: about 70 minutes, the most a job may capture.
const MAXIMUM_PCM_BYTES: usize = super::ffmpeg::MAXIMUM_JOB_STDOUT_BYTES;
const SCRATCH_EXTENSION: &str = "m4a";

/// audiowaveform's JSON format (version 2, one channel, 8-bit).
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Waveform {
    pub version: u8,
    pub channels: u8,
    pub sample_rate: u32,
    pub samples_per_pixel: usize,
    pub bits: u8,
    pub length: usize,
    /// `length` pairs of (lowest, highest) sample, -128 to 127.
    pub data: Vec<i8>,
}

/// Builds waveforms with its own FFmpeg slots (so playback audio never waits
/// for one), using `scratch` for the moment the audio is on its own.
pub struct WaveformBuilder {
    scratch: PathBuf,
    ffmpeg: Arc<dyn FfmpegExecutor>,
}

impl WaveformBuilder {
    pub fn new(scratch: PathBuf, ffmpeg: Arc<dyn FfmpegExecutor>) -> Self {
        // Left by an earlier run (or by an older build that kept waveforms).
        if let Ok(entries) = fs::read_dir(&scratch) {
            for entry in entries.flatten() {
                let _ = fs::remove_file(entry.path());
            }
        }
        Self { scratch, ffmpeg }
    }

    /// The waveform of the tracks at `indexes` of the clip at `path`, as JSON.
    pub(super) fn build(&self, path: &Path, indexes: &[u8]) -> AppResult<Vec<u8>> {
        let waveform = if indexes.is_empty() { pairs(&[]) } else { self.mixed(path, indexes)? };
        serde_json::to_vec(&waveform).map_err(|error| AppError::Integration(error.to_string()))
    }

    fn mixed(&self, path: &Path, indexes: &[u8]) -> AppResult<Waveform> {
        fs::create_dir_all(&self.scratch).map_err(|source| AppError::Io {
            action: "prepare a clip waveform",
            path: self.scratch.clone(),
            source,
        })?;
        let audio = self.scratch.join(format!("{}.{SCRATCH_EXTENSION}", random_token()));
        let result = extract_audio(path, &audio).and_then(|tracks| {
            if indexes.iter().any(|index| usize::from(*index) >= tracks) {
                return Err(AppError::Path("the clip has fewer audio tracks than expected".into()));
            }
            let decoded: Vec<AppResult<Vec<i16>>> = std::thread::scope(|scope| {
                let jobs: Vec<_> = indexes
                    .iter()
                    .map(|index| {
                        let audio = &audio;
                        scope.spawn(move || decode(self.ffmpeg.as_ref(), audio, *index))
                    })
                    .collect();
                jobs.into_iter()
                    .map(|job| job.join().unwrap_or_else(|_| Err(AppError::Integration("waveform decoding failed".into()))))
                    .collect()
            });
            Ok(pairs(&mix(&decoded.into_iter().collect::<AppResult<Vec<_>>>()?)))
        });
        let _ = fs::remove_file(&audio);
        result
    }
}

/// One track of the audio-only file at `audio`, as `RATE` mono samples from
/// the start of the clip (a late track starts with silence, as it plays).
fn decode(ffmpeg: &dyn FfmpegExecutor, audio: &Path, index: u8) -> AppResult<Vec<i16>> {
    // Timestamps as the clip has them: without the video, FFmpeg would
    // otherwise start the file at its earliest audio and pull every track
    // early by that much.
    let head = ["-nostdin", "-hide_banner", "-loglevel", "error", "-threads", "1", "-copyts"].map(OsString::from);
    let tail = [
        "-map".to_owned(),
        format!("0:a:{index}"),
        "-af".to_owned(),
        format!("aresample={RATE}:async=1:first_pts=0"),
        "-ac".to_owned(),
        "1".to_owned(),
        "-f".to_owned(),
        "s16le".to_owned(),
        "pipe:1".to_owned(),
    ]
    .map(OsString::from);
    let mut job = FfmpegJob::new("audio waveform", head.into_iter().chain(library_input(audio)).chain(tail));
    job.timeout = Duration::from_secs(120);
    job.maximum_stdout_bytes = MAXIMUM_PCM_BYTES;
    let output = ffmpeg.run(job)?;
    if !output.success {
        return Err(AppError::Integration(if output.stderr.is_empty() {
            format!("waveform FFmpeg exited with code {:?}", output.exit_code)
        } else {
            output.stderr
        }));
    }
    Ok(output.stdout.chunks_exact(2).map(|pair| i16::from_le_bytes([pair[0], pair[1]])).collect())
}

/// The tracks summed as playback hears them (unnormalised), clipped to range.
fn mix(tracks: &[Vec<i16>]) -> Vec<i16> {
    let len = tracks.iter().map(Vec::len).max().unwrap_or(0);
    (0..len)
        .map(|index| {
            let sum: i32 = tracks.iter().filter_map(|track| track.get(index)).map(|sample| i32::from(*sample)).sum();
            sum.clamp(i32::from(i16::MIN), i32::from(i16::MAX)) as i16
        })
        .collect()
}

/// Mono samples to (lowest, highest) pairs.
fn pairs(samples: &[i16]) -> Waveform {
    let per_pair = SAMPLES_PER_PIXEL.max(samples.len().div_ceil(MAXIMUM_PAIRS));
    let scale = |sample: i16| (sample >> 8) as i8;
    let mut data = Vec::with_capacity(samples.len().div_ceil(per_pair) * 2);
    for part in samples.chunks(per_pair) {
        data.push(scale(part.iter().copied().min().unwrap_or(0)));
        data.push(scale(part.iter().copied().max().unwrap_or(0)));
    }
    Waveform {
        version: 2,
        channels: 1,
        sample_rate: RATE,
        samples_per_pixel: per_pair,
        bits: 8,
        length: data.len() / 2,
        data,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pairs_keep_the_shape_of_every_10_ms() {
        // Two seconds: silence, then a full-scale square wave for 10 ms.
        let mut samples = vec![0_i16; RATE as usize * 2];
        for (offset, sample) in samples[RATE as usize..RATE as usize + 80].iter_mut().enumerate() {
            *sample = if offset % 2 == 0 { i16::MAX } else { i16::MIN };
        }
        let waveform = pairs(&samples);
        assert_eq!((waveform.version, waveform.channels, waveform.bits), (2, 1, 8));
        assert_eq!((waveform.sample_rate, waveform.samples_per_pixel, waveform.length), (8_000, 80, 200));
        assert_eq!(&waveform.data[200..202], &[-128, 127]);
        assert_eq!(waveform.data.iter().filter(|value| **value != 0).count(), 2);
    }

    #[test]
    fn long_audio_gets_coarser_pairs_and_none_is_empty() {
        let waveform = pairs(&vec![0_i16; RATE as usize * 600]);
        assert!(waveform.length <= MAXIMUM_PAIRS);
        assert_eq!(waveform.samples_per_pixel, 120);
        assert_eq!(pairs(&[]).length, 0);
    }

    #[test]
    fn tracks_sum_like_playback_and_clip_at_full_scale() {
        let mixed = mix(&[vec![1_000, 30_000, -30_000], vec![500, 10_000, -10_000, 7]]);
        assert_eq!(mixed, vec![1_500, i16::MAX, i16::MIN, 7]);
        assert!(mix(&[]).is_empty());
    }

    /// Answers each track with a fixed level, and checks what is asked.
    struct Tracks;
    impl FfmpegExecutor for Tracks {
        fn run(&self, job: FfmpegJob) -> AppResult<super::super::FfmpegOutput> {
            let args: Vec<String> = job.args.iter().map(|arg| arg.to_string_lossy().into_owned()).collect();
            let map = &args[args.iter().position(|arg| arg == "-map").unwrap() + 1];
            let input = &args[args.iter().position(|arg| arg == "-i").unwrap() + 1];
            assert!(Path::new(input).exists(), "the audio-only file is there while decoding");
            assert!(args.iter().any(|arg| arg == "-copyts"), "tracks keep the clip's timing");
            let level: i16 = if map == "0:a:0" { 256 } else { -512 };
            Ok(super::super::FfmpegOutput {
                success: true,
                exit_code: Some(0),
                stdout: [level, 0].iter().flat_map(|sample| sample.to_le_bytes()).collect(),
                stderr: String::new(),
            })
        }
    }

    #[test]
    fn a_waveform_is_built_from_the_audio_alone_and_nothing_is_kept() {
        let dir = tempfile::tempdir().unwrap();
        let clip = super::super::trim::tests::fixture(dir.path());
        let scratch = dir.path().join("waveforms");
        let builder = WaveformBuilder::new(scratch.clone(), Arc::new(Tracks));
        let json: serde_json::Value = serde_json::from_slice(&builder.build(&clip.path, &[0]).unwrap()).unwrap();
        assert_eq!(json["data"], serde_json::json!([0, 1]));
        assert!(builder.build(&clip.path, &[0, 1]).is_err(), "the fixture has one audio track");
        assert_eq!(fs::read_dir(&scratch).unwrap().count(), 0, "the audio-only file is gone");
    }
}

