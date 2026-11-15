//! The on-disk queue (ADR 0035, "Client storage"): append-only JSON-lines
//! segments in `telemetry\journal`. A record is written to the OS when it is
//! produced, so a crash or abort keeps it; segments are sealed for upload and
//! deleted only after the server has them.
use clipture_telemetry_protocol::{Body, Record};
use serde::{Deserialize, Serialize};
use std::{
    fs::{self, File, OpenOptions},
    io::{BufRead, BufReader, Write},
    path::{Path, PathBuf},
    time::{Duration, Instant},
};

const SEAL_BYTES: u64 = 256 * 1024;
pub const SEAL_AGE: Duration = Duration::from_secs(60);
const MAXIMUM_BYTES: u64 = 8 * 1024 * 1024;
const OPEN_SUFFIX: &str = ".open.jsonl";
const SEALED_SUFFIX: &str = ".jsonl";

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct JournalState {
    pub next_seq: u64,
    pub last_run: Option<String>,
    pub last_run_ended: bool,
    pub last_version: Option<String>,
    pub crash_log_offset: u64,
}

pub struct Segment {
    pub path: PathBuf,
    pub first_seq: u64,
}

pub struct Journal {
    directory: PathBuf,
    state_file: PathBuf,
    pub state: JournalState,
    open: Option<(File, PathBuf, u64, Instant)>,
}

fn segment_seq(path: &Path) -> Option<u64> {
    let name = path.file_name()?.to_str()?;
    name.strip_suffix(OPEN_SUFFIX).or_else(|| name.strip_suffix(SEALED_SUFFIX))?.parse().ok()
}

fn last_seq_in(path: &Path) -> Option<u64> {
    let file = File::open(path).ok()?;
    BufReader::new(file)
        .lines()
        .map_while(Result::ok)
        .filter_map(|line| serde_json::from_str::<serde_json::Value>(&line).ok()?["seq"].as_u64())
        .max()
}

impl Journal {
    /// Opens the journal, sealing a segment a previous run left open, and
    /// never reuses a sequence number already on disk.
    pub fn open(root: &Path) -> std::io::Result<Self> {
        let directory = root.join("journal");
        fs::create_dir_all(&directory)?;
        let state_file = root.join("state.json");
        let state: JournalState = fs::read(&state_file).ok().and_then(|bytes| serde_json::from_slice(&bytes).ok()).unwrap_or_default();
        let mut journal = Self { directory, state_file, state, open: None };
        let mut highest = journal.state.next_seq.saturating_sub(1);
        for entry in fs::read_dir(&journal.directory)?.flatten() {
            let path = entry.path();
            if let Some(seq) = last_seq_in(&path) {
                highest = highest.max(seq);
            }
            if path.to_string_lossy().ends_with(OPEN_SUFFIX) {
                journal.seal_path(&path)?;
            }
        }
        journal.state.next_seq = journal.state.next_seq.max(highest + 1).max(1);
        journal.save_state()?;
        Ok(journal)
    }

    pub fn save_state(&self) -> std::io::Result<()> {
        let temporary = self.state_file.with_extension("json.tmp");
        fs::write(&temporary, serde_json::to_vec_pretty(&self.state).unwrap_or_default())?;
        fs::rename(temporary, &self.state_file)
    }

    pub fn append(&mut self, run: &str, session: Option<&str>, t: String, body: Body) -> std::io::Result<u64> {
        let seq = self.state.next_seq;
        let record = Record { seq, t, run: run.to_string(), session: session.map(str::to_string), body };
        let mut line = serde_json::to_vec(&record).map_err(std::io::Error::other)?;
        line.push(b'\n');
        if self.open.is_none() {
            let path = self.directory.join(format!("{seq:020}{OPEN_SUFFIX}"));
            let file = OpenOptions::new().create(true).append(true).open(&path)?;
            self.open = Some((file, path, 0, Instant::now()));
        }
        let (file, _, size, _) = self.open.as_mut().expect("opened above");
        file.write_all(&line)?;
        *size += line.len() as u64;
        self.state.next_seq += 1;
        if *size >= SEAL_BYTES {
            self.seal()?;
        }
        Ok(seq)
    }

    /// How long the open segment has had records, if it has any.
    pub fn open_age(&self) -> Option<Duration> {
        self.open.as_ref().map(|(_, _, _, opened)| opened.elapsed())
    }

    pub fn seal(&mut self) -> std::io::Result<()> {
        if let Some((file, path, _, _)) = self.open.take() {
            file.sync_all()?;
            drop(file);
            self.seal_path(&path)?;
        }
        self.save_state()?;
        self.enforce_cap()
    }

    fn seal_path(&self, path: &Path) -> std::io::Result<()> {
        let name = path.file_name().and_then(|name| name.to_str()).unwrap_or_default();
        let sealed = self.directory.join(name.replace(OPEN_SUFFIX, SEALED_SUFFIX));
        if fs::metadata(path)?.len() == 0 {
            return fs::remove_file(path);
        }
        fs::rename(path, sealed)
    }

    /// Sealed segments, oldest first.
    pub fn sealed(&self) -> Vec<Segment> {
        let mut segments: Vec<Segment> = fs::read_dir(&self.directory)
            .into_iter()
            .flatten()
            .flatten()
            .map(|entry| entry.path())
            .filter(|path| path.to_string_lossy().ends_with(SEALED_SUFFIX) && !path.to_string_lossy().ends_with(OPEN_SUFFIX))
            .filter_map(|path| Some(Segment { first_seq: segment_seq(&path)?, path }))
            .collect();
        segments.sort_by_key(|segment| segment.first_seq);
        segments
    }

    /// Past the cap the oldest segments go, and a `gap` says which records.
    fn enforce_cap(&mut self) -> std::io::Result<()> {
        loop {
            let segments = self.sealed();
            let total: u64 = segments.iter().filter_map(|s| fs::metadata(&s.path).ok()).map(|m| m.len()).sum();
            if total <= MAXIMUM_BYTES || segments.len() < 2 {
                return Ok(());
            }
            self.drop_segment(&segments[0], "journal full")?;
        }
    }

    /// Deletes a segment that will never be sent and records the hole.
    pub fn drop_segment(&mut self, segment: &Segment, reason: &str) -> std::io::Result<()> {
        let to_seq = last_seq_in(&segment.path).unwrap_or(segment.first_seq);
        fs::remove_file(&segment.path)?;
        let run = self.state.last_run.clone().unwrap_or_default();
        self.append(&run, None, super::now_text(), Body::Gap { from_seq: segment.first_seq, to_seq, reason: reason.into() })?;
        Ok(())
    }

    pub fn remove_sent(&self, segment: &Segment) -> std::io::Result<()> {
        fs::remove_file(&segment.path)
    }

    pub fn pending_bytes(&self) -> u64 {
        fs::read_dir(&self.directory).into_iter().flatten().flatten().filter_map(|e| e.metadata().ok()).map(|m| m.len()).sum()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn event(code: &str) -> Body {
        Body::Event(clipture_telemetry_protocol::Event::new(clipture_telemetry_protocol::Level::Info, code))
    }

    fn lines(path: &Path) -> Vec<Record> {
        fs::read_to_string(path).unwrap().lines().map(|line| serde_json::from_str(line).unwrap()).collect()
    }

    #[test]
    fn appends_seals_and_numbers_records_in_order() {
        let root = tempfile::tempdir().unwrap();
        let mut journal = Journal::open(root.path()).unwrap();
        assert_eq!(journal.append("r", None, "t".into(), event("a")).unwrap(), 1);
        assert_eq!(journal.append("r", Some("s"), "t".into(), event("b")).unwrap(), 2);
        assert!(journal.sealed().is_empty(), "the open segment is not offered for upload");
        journal.seal().unwrap();
        let sealed = journal.sealed();
        assert_eq!(sealed.len(), 1);
        assert_eq!(sealed[0].first_seq, 1);
        let records = lines(&sealed[0].path);
        assert_eq!(records.iter().map(|r| r.seq).collect::<Vec<_>>(), [1, 2]);
        assert_eq!(records[1].session.as_deref(), Some("s"));
    }

    #[test]
    fn a_crashed_run_loses_nothing_and_never_reuses_numbers() {
        let root = tempfile::tempdir().unwrap();
        {
            let mut journal = Journal::open(root.path()).unwrap();
            for code in ["a", "b", "c"] {
                journal.append("r", None, "t".into(), event(code)).unwrap();
            }
            // Dropped without seal() or save_state(): as after an abort.
            std::mem::forget(journal);
        }
        let mut journal = Journal::open(root.path()).unwrap();
        let sealed = journal.sealed();
        assert_eq!(sealed.len(), 1, "the open segment was sealed on start");
        assert_eq!(lines(&sealed[0].path).len(), 3);
        assert_eq!(journal.append("r2", None, "t".into(), event("d")).unwrap(), 4);
    }

    #[test]
    fn sent_segments_go_and_dropped_ones_leave_a_gap() {
        let root = tempfile::tempdir().unwrap();
        let mut journal = Journal::open(root.path()).unwrap();
        journal.append("r", None, "t".into(), event("a")).unwrap();
        journal.seal().unwrap();
        journal.append("r", None, "t".into(), event("b")).unwrap();
        journal.append("r", None, "t".into(), event("c")).unwrap();
        journal.seal().unwrap();
        let sealed = journal.sealed();
        journal.remove_sent(&sealed[0]).unwrap();
        journal.drop_segment(&sealed[1], "rejected").unwrap();
        journal.seal().unwrap();
        let sealed = journal.sealed();
        assert_eq!(sealed.len(), 1);
        let gap = &lines(&sealed[0].path)[0];
        assert_eq!(gap.body, Body::Gap { from_seq: 2, to_seq: 3, reason: "rejected".into() });
    }

    #[test]
    fn the_cap_drops_the_oldest_segments() {
        let root = tempfile::tempdir().unwrap();
        let mut journal = Journal::open(root.path()).unwrap();
        let big = clipture_telemetry_protocol::Event::new(clipture_telemetry_protocol::Level::Info, "x").message("y".repeat(4000));
        // 40 sealed segments of ~256 KB is past the 8 MB cap.
        for _ in 0..(40 * 64) {
            journal.append("r", None, "t".into(), Body::Event(big.clone())).unwrap();
        }
        journal.seal().unwrap();
        assert!(journal.pending_bytes() <= MAXIMUM_BYTES + SEAL_BYTES);
        let first = journal.sealed()[0].first_seq;
        assert!(first > 1, "oldest records were dropped");
    }
}
