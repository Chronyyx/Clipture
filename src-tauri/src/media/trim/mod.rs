//! Destructive in-place trimming of Clipture's own MP4 clips. The removed
//! footage is deleted, not hidden: kept samples are packed down over it, the
//! index is rewritten, and the file is cut to its new length. Nothing is
//! re-encoded and no second copy of the clip is made; the only extra disk
//! use is a journal holding the new index and at most one 16 MB block.
//!
//! A trim either never starts changing the clip or always finishes: the plan
//! is journaled before the first byte moves, and an interrupted trim (crash,
//! power loss) is completed from the journal the next time Clipture starts.
mod apply;
pub(super) mod disk;
mod index;
mod journal;
mod layout;
#[cfg(test)]
pub(super) mod tests;

use std::{
    io::{self, Read, Seek, SeekFrom},
    path::{Path, PathBuf},
    sync::Mutex,
    time::Duration,
};

use super::mp4_box::top_level_boxes;
use super::{layout_probe::layout_from_moov, random_token};

const MAXIMUM_INDEX_BYTES: u64 = 64 * 1024 * 1024;
/// How long a trim waits for the recorder (or a player) to let go of a clip.
const IN_USE_RETRIES: u32 = 20;
const IN_USE_WAIT: Duration = Duration::from_millis(250);
const JOURNAL_EXTENSION: &str = "trim";

#[derive(Debug, thiserror::Error)]
pub enum TrimError {
    #[error("{0}")]
    Unsupported(&'static str),
    #[error("{0}")]
    Range(&'static str),
    #[error("Clipture is still using this clip. Try again in a moment.")]
    Busy,
    #[error("the clip could not be read or written: {0}")]
    Io(#[from] io::Error),
    #[error("the trim stopped partway ({0}); it finishes the next time Clipture starts")]
    Interrupted(io::Error),
}

/// A finished trim.
#[derive(Clone, Debug, PartialEq)]
pub struct TrimmedClip {
    pub path: PathBuf,
    pub duration_seconds: f64,
    pub bytes_before: u64,
    pub bytes_after: u64,
}

pub struct ClipTrimmer {
    journals: PathBuf,
    /// One trim at a time: journals and clips are never shared between two.
    running: Mutex<()>,
}

impl ClipTrimmer {
    /// Journals live in `journals` (inside Clipture's data folder).
    pub fn new(journals: PathBuf) -> Self {
        Self { journals, running: Mutex::new(()) }
    }

    /// Keeps `[start, end)` seconds of the clip at `path` and deletes the
    /// rest. Blocking; seconds to minutes depending on how much is kept.
    pub fn trim(&self, path: &Path, start: f64, end: f64) -> Result<TrimmedClip, TrimError> {
        let _running = self.running.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        // A trim cut short earlier must finish before anything else moves.
        self.finish_locked();
        let mut clip = open_waiting(path)?;
        let clip_len = disk::Disk::len(&mut clip)?;
        let plan = plan_for(&mut clip, path, clip_len, start, end)?;

        std::fs::create_dir_all(&self.journals)?;
        let journal_path = self.journals.join(format!("{}.{JOURNAL_EXTENSION}", random_token()));
        let mut journal_file = disk::open_exclusive(&journal_path, true)?;
        let committed = journal::commit(&mut journal_file, &plan);
        let (plan_len, progress) = match committed {
            Ok(committed) => committed,
            Err(error) => {
                // Not committed: the clip is untouched.
                drop(journal_file);
                let _ = std::fs::remove_file(&journal_path);
                return Err(error.into());
            }
        };
        if let Err(first) = apply::run(&mut clip, &mut journal_file, &plan, plan_len, progress) {
            drop((clip, journal_file));
            // Retry once from the journal, exactly as recovery would.
            return match finish_journal(&journal_path) {
                Ok(Some(done)) => Ok(done),
                _ => Err(TrimError::Interrupted(first)),
            };
        }
        drop((clip, journal_file));
        let _ = std::fs::remove_file(&journal_path);
        Ok(trimmed(&plan))
    }

    /// Completes trims a crash interrupted. Call before anything reads clips.
    pub fn finish_interrupted(&self) -> Vec<TrimmedClip> {
        let _running = self.running.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        self.finish_locked()
    }

    fn finish_locked(&self) -> Vec<TrimmedClip> {
        let Ok(entries) = std::fs::read_dir(&self.journals) else {
            return Vec::new();
        };
        let mut finished = Vec::new();
        for path in entries.flatten().map(|entry| entry.path()) {
            if path.extension().is_none_or(|extension| extension != JOURNAL_EXTENSION) {
                continue;
            }
            match finish_journal(&path) {
                Ok(Some(done)) => {
                    tracing::info!(clip = %done.path.display(), "finished an interrupted trim");
                    finished.push(done);
                }
                Ok(None) => {}
                Err(error) => tracing::warn!(%error, journal = %path.display(), "could not finish an interrupted trim"),
            }
        }
        finished
    }
}

/// Runs a journal to completion and deletes it. `None` when it was never
/// committed (the clip was not touched) or its clip is gone.
fn finish_journal(path: &Path) -> io::Result<Option<TrimmedClip>> {
    let mut journal_file = disk::open_exclusive(path, false)?;
    let Some((plan, plan_len, progress)) = journal::read(&mut journal_file)? else {
        drop(journal_file);
        std::fs::remove_file(path)?;
        return Ok(None);
    };
    let mut clip = match open_waiting(&plan.clip) {
        Ok(clip) => clip,
        Err(TrimError::Io(error)) if error.kind() == io::ErrorKind::NotFound => {
            tracing::warn!(clip = %plan.clip.display(), "the clip of an interrupted trim is gone");
            drop(journal_file);
            std::fs::remove_file(path)?;
            return Ok(None);
        }
        Err(TrimError::Io(error)) => return Err(error),
        Err(error) => return Err(io::Error::other(error.to_string())),
    };
    apply::run(&mut clip, &mut journal_file, &plan, plan_len, progress)?;
    drop((clip, journal_file));
    std::fs::remove_file(path)?;
    Ok(Some(trimmed(&plan)))
}

fn trimmed(plan: &layout::Plan) -> TrimmedClip {
    TrimmedClip {
        path: plan.clip.clone(),
        duration_seconds: plan.duration_ms as f64 / 1000.0,
        bytes_before: plan.clip_len,
        bytes_after: plan.final_len,
    }
}

fn open_waiting(path: &Path) -> Result<std::fs::File, TrimError> {
    for _ in 0..IN_USE_RETRIES {
        match disk::open_exclusive(path, false) {
            Ok(file) => return Ok(file),
            Err(error) if disk::is_in_use(&error) => std::thread::sleep(IN_USE_WAIT),
            Err(error) => return Err(error.into()),
        }
    }
    Err(TrimError::Busy)
}

/// Reads the clip's layout and index and plans the trim, checking the
/// result describes a valid file before anything is committed.
fn plan_for(clip: &mut std::fs::File, path: &Path, clip_len: u64, start: f64, end: f64) -> Result<layout::Plan, TrimError> {
    let top = top_level_boxes(clip, clip_len)?.ok_or(TrimError::Unsupported("the clip's boxes are malformed"))?;
    let moov = top
        .iter()
        .find(|item| &item.kind == b"moov")
        .ok_or(TrimError::Unsupported("the clip has no index"))?;
    if moov.size > MAXIMUM_INDEX_BYTES {
        return Err(TrimError::Unsupported("the clip's index is too large"));
    }
    let mut bytes = vec![0_u8; moov.size as usize];
    clip.seek(SeekFrom::Start(moov.at))?;
    clip.read_exact(&mut bytes)?;
    let (selection, movie_scale) = index::select(&bytes, start, end)?;
    let plan = layout::plan(path, clip_len, &top, &selection, movie_scale)?;
    check(&plan)?;
    Ok(plan)
}

/// The planned file's own index must place every sample inside the packed
/// media, back to back, covering it exactly.
fn check(plan: &layout::Plan) -> Result<(), TrimError> {
    let broken = TrimError::Unsupported("the trimmed index did not check out");
    let tracks = layout_from_moov(&plan.moov).ok_or(TrimError::Unsupported("the trimmed index did not check out"))?;
    let media_start = plan.mdat_header_at + plan.mdat_header.len() as u64;
    let mut spans: Vec<(u64, u64)> = tracks
        .iter()
        .flat_map(|track| track.offsets.iter().zip(&track.sizes).map(|(offset, size)| (*offset, u64::from(*size))))
        .filter(|(_, size)| *size > 0)
        .collect();
    spans.sort_unstable();
    let mut cursor = media_start;
    for (offset, size) in spans {
        if offset != cursor {
            return Err(broken);
        }
        cursor += size;
    }
    if cursor != plan.moov_at {
        return Err(TrimError::Unsupported("the trimmed index did not check out"));
    }
    Ok(())
}
