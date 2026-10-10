//! Destructive per-track audio levels. Each audio track whose level changes
//! is re-encoded with its gain applied (a muted track becomes silence) and
//! written back into the clip in place; the video and unchanged tracks are
//! not touched. The new packets go into the old ones' slots, in time order,
//! so the clip keeps its interleaving; whatever a slot has left over is
//! zeroed, so the old audio is gone. A little overflow goes at the end of
//! the media. Only the audio of the changed tracks is written, and no copy
//! of the clip is made.
//!
//! The new bytes, the new index and the final length are journaled before
//! the clip changes. Every journaled write is plain data (nothing is read
//! from the clip while applying), so an interrupted mix is finished by
//! applying the journal again the next time Clipture starts.
mod encode;
mod journal;
mod plan;
#[cfg(test)]
mod tests;

use std::{
    io::{self, Read, Seek, SeekFrom},
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};

use super::mp4_box::top_level_boxes;
use super::random_token;
use super::trim::disk::{self, Disk};
use super::FfmpegExecutor;

const MAXIMUM_INDEX_BYTES: u64 = 64 * 1024 * 1024;
const IN_USE_RETRIES: u32 = 20;
const IN_USE_WAIT: std::time::Duration = std::time::Duration::from_millis(250);
const JOURNAL_EXTENSION: &str = "mix";
/// Levels are a gain on each track: 0 is silence, 1 leaves it as is, 10 is
/// +20 dB (the mixer's faders work in decibels).
pub const MAXIMUM_GAIN: f64 = 10.0;
/// Closer to 1 than this is "unchanged": the track is not re-encoded.
const UNCHANGED: f64 = 0.005;

#[derive(Debug, thiserror::Error)]
pub enum MixError {
    #[error("{0}")]
    Unsupported(&'static str),
    #[error("Clipture is still using this clip. Try again in a moment.")]
    Busy,
    #[error("the clip changed while its audio was being prepared; try again")]
    Changed,
    #[error("the audio could not be re-encoded: {0}")]
    Encode(String),
    #[error("the clip could not be read or written: {0}")]
    Io(#[from] io::Error),
    #[error("the mix stopped partway ({0}); it finishes the next time Clipture starts")]
    Interrupted(io::Error),
}

/// A finished mix.
#[derive(Clone, Debug, PartialEq)]
pub struct MixedClip {
    pub path: PathBuf,
    pub tracks_changed: usize,
    pub bytes_before: u64,
    pub bytes_after: u64,
}

pub struct ClipMixer {
    journals: PathBuf,
    ffmpeg: Arc<dyn FfmpegExecutor>,
    /// One mix at a time: journals and clips are never shared between two.
    running: Mutex<()>,
}

impl ClipMixer {
    /// Journals (and the short-lived encoded audio) live in `journals`.
    pub fn new(journals: PathBuf, ffmpeg: Arc<dyn FfmpegExecutor>) -> Self {
        Self { journals, ffmpeg, running: Mutex::new(()) }
    }

    /// Applies `gains[k]` to the clip's k-th audio track for good. Blocking;
    /// about a second per changed track for a two-minute clip.
    pub fn mix(&self, path: &Path, gains: &[f64]) -> Result<MixedClip, MixError> {
        let _running = self.running.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        self.finish_locked();
        if gains.iter().any(|gain| !gain.is_finite() || !(0.0..=MAXIMUM_GAIN).contains(gain)) {
            return Err(MixError::Unsupported("a track level is out of range"));
        }
        // Read the index while sharing the clip: FFmpeg reads it too.
        let (moov, clip_len) = read_index(&mut std::fs::File::open(path)?)?;
        let tracks = plan::audio_tracks(&moov)?;
        if gains.len() != tracks.len() {
            return Err(MixError::Unsupported("the levels do not match the clip's audio tracks"));
        }
        let changed: Vec<(usize, f64)> = gains
            .iter()
            .copied()
            .enumerate()
            .filter(|(_, gain)| (gain - 1.0).abs() > UNCHANGED)
            .collect();
        if changed.is_empty() {
            return Ok(MixedClip { path: path.to_owned(), tracks_changed: 0, bytes_before: clip_len, bytes_after: clip_len });
        }

        std::fs::create_dir_all(&self.journals)?;
        let token = random_token();
        let encoded_path = self.journals.join(format!("{token}.m4a"));
        let encoded = encode::encode(self.ffmpeg.as_ref(), path, &encoded_path, &tracks, &changed);
        let _ = std::fs::remove_file(&encoded_path);
        let encoded = encoded?;

        let mut clip = open_waiting(path)?;
        let (moov_now, len_now) = read_index(&mut clip)?;
        if moov_now != moov || len_now != clip_len {
            return Err(MixError::Changed);
        }
        let top = top_level_boxes(&mut clip, clip_len)?.ok_or(MixError::Unsupported("the clip's boxes are malformed"))?;
        let plan = plan::plan(path, clip_len, &top, &moov, &tracks, &changed, &encoded)?;

        let journal_path = self.journals.join(format!("{token}.{JOURNAL_EXTENSION}"));
        let mut journal_file = disk::open_exclusive(&journal_path, true)?;
        if let Err(error) = journal::commit(&mut journal_file, &plan) {
            drop(journal_file);
            let _ = std::fs::remove_file(&journal_path);
            return Err(error.into());
        }
        if let Err(first) = apply(&mut clip, &plan) {
            drop((clip, journal_file));
            return match finish_journal(&journal_path) {
                Ok(Some(done)) => Ok(done),
                _ => Err(MixError::Interrupted(first)),
            };
        }
        drop((clip, journal_file));
        let _ = std::fs::remove_file(&journal_path);
        Ok(mixed(&plan))
    }

    /// Completes mixes a crash interrupted. Call before anything reads clips.
    pub fn finish_interrupted(&self) -> Vec<MixedClip> {
        let _running = self.running.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        self.finish_locked()
    }

    fn finish_locked(&self) -> Vec<MixedClip> {
        let Ok(entries) = std::fs::read_dir(&self.journals) else {
            return Vec::new();
        };
        let mut finished = Vec::new();
        for path in entries.flatten().map(|entry| entry.path()) {
            match path.extension().and_then(|extension| extension.to_str()) {
                Some(JOURNAL_EXTENSION) => match finish_journal(&path) {
                    Ok(Some(done)) => {
                        tracing::info!(clip = %done.path.display(), "finished an interrupted audio mix");
                        finished.push(done);
                    }
                    Ok(None) => {}
                    Err(error) => tracing::warn!(%error, journal = %path.display(), "could not finish an interrupted audio mix"),
                },
                // Encoded audio left by a mix that never committed.
                Some("m4a") => {
                    let _ = std::fs::remove_file(&path);
                }
                _ => {}
            }
        }
        finished
    }
}

/// Applies a journal and deletes it. `None` when it was never committed (the
/// clip was not touched) or its clip is gone.
fn finish_journal(path: &Path) -> io::Result<Option<MixedClip>> {
    let mut journal_file = disk::open_exclusive(path, false)?;
    let Some(plan) = journal::read(&mut journal_file)? else {
        drop(journal_file);
        std::fs::remove_file(path)?;
        return Ok(None);
    };
    let mut clip = match open_waiting(&plan.clip) {
        Ok(clip) => clip,
        Err(MixError::Io(error)) if error.kind() == io::ErrorKind::NotFound => {
            tracing::warn!(clip = %plan.clip.display(), "the clip of an interrupted audio mix is gone");
            drop(journal_file);
            std::fs::remove_file(path)?;
            return Ok(None);
        }
        Err(MixError::Io(error)) => return Err(error),
        Err(error) => return Err(io::Error::other(error.to_string())),
    };
    apply(&mut clip, &plan)?;
    drop((clip, journal_file));
    std::fs::remove_file(path)?;
    Ok(Some(mixed(&plan)))
}

/// Every write is data from the plan, so applying twice is harmless.
fn apply(clip: &mut impl Disk, plan: &plan::Plan) -> io::Result<()> {
    if plan.final_len > plan.clip_len {
        clip.set_len(plan.final_len)?;
    }
    for write in &plan.writes {
        clip.write_at(write.at, &write.bytes)?;
    }
    clip.set_len(plan.final_len)?;
    clip.sync()
}

fn mixed(plan: &plan::Plan) -> MixedClip {
    MixedClip {
        path: plan.clip.clone(),
        tracks_changed: plan.tracks_changed,
        bytes_before: plan.clip_len,
        bytes_after: plan.final_len,
    }
}

fn open_waiting(path: &Path) -> Result<std::fs::File, MixError> {
    for _ in 0..IN_USE_RETRIES {
        match disk::open_exclusive(path, false) {
            Ok(file) => return Ok(file),
            Err(error) if disk::is_in_use(&error) => std::thread::sleep(IN_USE_WAIT),
            Err(error) => return Err(error.into()),
        }
    }
    Err(MixError::Busy)
}

/// The clip's `moov` box (header included) and its length.
fn read_index(clip: &mut std::fs::File) -> Result<(Vec<u8>, u64), MixError> {
    let clip_len = clip.metadata()?.len();
    let top = top_level_boxes(clip, clip_len)?.ok_or(MixError::Unsupported("the clip's boxes are malformed"))?;
    let moov = top
        .iter()
        .find(|item| &item.kind == b"moov")
        .ok_or(MixError::Unsupported("the clip has no index"))?;
    if moov.size > MAXIMUM_INDEX_BYTES {
        return Err(MixError::Unsupported("the clip's index is too large"));
    }
    let mut bytes = vec![0_u8; moov.size as usize];
    clip.seek(SeekFrom::Start(moov.at))?;
    clip.read_exact(&mut bytes)?;
    Ok((bytes, clip_len))
}
