//! What friends have read of the clips we shared, so the sender can see it.
//! Counted as bytes leave this PC, from any stream; runtime only. A friend
//! may watch, then keep: progress is the union of every byte read once.
use std::{
    collections::{HashMap, VecDeque},
    sync::Mutex,
    time::{Duration, Instant},
};

use super::model::{TransferPurpose, TransferState, TransferView};

/// A transfer with bytes this recent counts as active.
const ACTIVE_WINDOW: Duration = Duration::from_secs(2);
/// Speed is averaged over this much recent sending.
const RATE_WINDOW: Duration = Duration::from_secs(3);
/// The UI is told about progress at most this often per share.
const NOTIFY_INTERVAL: Duration = Duration::from_millis(250);
const MAX_ENTRIES: usize = 64;

/// What changed, for the caller to announce (and to play a cue for).
#[derive(Debug, PartialEq, Eq)]
pub enum Change {
    /// Worth a UI refresh.
    Progress,
    /// The friend now has every byte.
    Complete,
    /// The connection closed while bytes were still being sent.
    Interrupted,
}

struct Entry {
    size: u64,
    /// Sorted, merged `[start, end)` spans that have been sent.
    spans: Vec<[u64; 2]>,
    purpose: TransferPurpose,
    streams: u32,
    last_byte: Instant,
    last_notice: Option<Instant>,
    recent: VecDeque<(Instant, u64)>,
    interrupted: bool,
}

impl Entry {
    fn sent(&self) -> u64 {
        self.spans.iter().map(|[start, end]| end - start).sum()
    }

    fn complete(&self) -> bool {
        self.spans.first() == Some(&[0, self.size])
    }

    fn add(&mut self, start: u64, end: u64) {
        let mut merged = [start, end];
        let mut kept = Vec::with_capacity(self.spans.len() + 1);
        for span in self.spans.drain(..) {
            if span[1] < merged[0] || span[0] > merged[1] {
                kept.push(span);
            } else {
                merged = [merged[0].min(span[0]), merged[1].max(span[1])];
            }
        }
        kept.push(merged);
        kept.sort_unstable();
        self.spans = kept;
    }
}

#[derive(Default)]
pub struct TransferBook {
    entries: Mutex<HashMap<String, Entry>>,
}

impl TransferBook {
    fn lock(&self) -> std::sync::MutexGuard<'_, HashMap<String, Entry>> {
        self.entries.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// A stream started sending bytes of `share_id`. `Keep` wins over
    /// `Watch`: once a friend downloads, that is what they are doing.
    pub fn begin(&self, share_id: &str, size: u64, purpose: Option<TransferPurpose>) -> Change {
        let mut entries = self.lock();
        if entries.len() >= MAX_ENTRIES && !entries.contains_key(share_id) {
            if let Some(oldest) = entries
                .iter()
                .filter(|(_, entry)| entry.streams == 0)
                .min_by_key(|(_, entry)| entry.last_byte)
                .map(|(id, _)| id.clone())
            {
                entries.remove(&oldest);
            }
        }
        let now = Instant::now();
        let entry = entries.entry(share_id.to_owned()).or_insert_with(|| Entry {
            size,
            spans: Vec::new(),
            purpose: purpose.unwrap_or(TransferPurpose::Watch),
            streams: 0,
            last_byte: now,
            last_notice: None,
            recent: VecDeque::new(),
            interrupted: false,
        });
        if purpose == Some(TransferPurpose::Keep) {
            entry.purpose = TransferPurpose::Keep;
        }
        entry.streams += 1;
        entry.interrupted = false;
        Change::Progress
    }

    /// `start..end` of the clip has been sent.
    pub fn sent(&self, share_id: &str, start: u64, end: u64) -> Option<Change> {
        let mut entries = self.lock();
        let entry = entries.get_mut(share_id)?;
        let was_complete = entry.complete();
        let now = Instant::now();
        entry.add(start, end.min(entry.size));
        entry.last_byte = now;
        entry.recent.push_back((now, end.saturating_sub(start)));
        while entry
            .recent
            .front()
            .is_some_and(|(at, _)| now.duration_since(*at) > RATE_WINDOW)
        {
            entry.recent.pop_front();
        }
        if !was_complete && entry.complete() {
            entry.last_notice = Some(now);
            return Some(Change::Complete);
        }
        let due = entry
            .last_notice
            .is_none_or(|at| now.duration_since(at) >= NOTIFY_INTERVAL);
        due.then(|| {
            entry.last_notice = Some(now);
            Change::Progress
        })
    }

    /// A stream ended. `connection_closed` means the whole connection went
    /// away mid-send (the friend quit, lost their network, or appeared
    /// offline), as opposed to the friend stopping one read.
    pub fn end(&self, share_id: &str, finished: bool, connection_closed: bool) -> Change {
        let mut entries = self.lock();
        let Some(entry) = entries.get_mut(share_id) else {
            return Change::Progress;
        };
        entry.streams = entry.streams.saturating_sub(1);
        entry.recent.clear();
        if !finished && connection_closed && !entry.complete() && !entry.interrupted {
            entry.interrupted = true;
            return Change::Interrupted;
        }
        Change::Progress
    }

    pub fn view(&self, share_id: &str) -> Option<TransferView> {
        let entries = self.lock();
        let entry = entries.get(share_id)?;
        let now = Instant::now();
        let state = if entry.complete() {
            TransferState::Complete
        } else if entry.interrupted {
            TransferState::Interrupted
        } else if entry.streams > 0 || now.duration_since(entry.last_byte) < ACTIVE_WINDOW {
            TransferState::Active
        } else {
            TransferState::Paused
        };
        let rate_bytes: u64 = entry.recent.iter().map(|(_, bytes)| bytes).sum();
        let span = entry
            .recent
            .front()
            .map_or(Duration::ZERO, |(at, _)| now.duration_since(*at))
            .max(Duration::from_millis(500));
        let bytes_per_second = if state == TransferState::Active {
            (rate_bytes as f64 / span.as_secs_f64()) as u64
        } else {
            0
        };
        Some(TransferView {
            sent_bytes: entry.sent(),
            total_bytes: entry.size,
            purpose: entry.purpose,
            state,
            bytes_per_second,
        })
    }

    pub fn forget(&self, share_id: &str) {
        self.lock().remove(share_id);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn progress_is_the_union_of_everything_sent() {
        let book = TransferBook::default();
        book.begin("s", 100, Some(TransferPurpose::Watch));
        book.sent("s", 0, 40);
        book.sent("s", 20, 60);
        book.sent("s", 80, 100);
        let view = book.view("s").unwrap();
        assert_eq!(view.sent_bytes, 80);
        assert_eq!(view.purpose, TransferPurpose::Watch);
        assert_eq!(view.state, TransferState::Active);

        book.begin("s", 100, Some(TransferPurpose::Keep));
        assert_eq!(book.sent("s", 60, 80), Some(Change::Complete));
        let view = book.view("s").unwrap();
        assert_eq!((view.sent_bytes, view.purpose), (100, TransferPurpose::Keep));
        assert_eq!(view.state, TransferState::Complete);
    }

    #[test]
    fn only_a_connection_closing_mid_send_interrupts() {
        let book = TransferBook::default();
        book.begin("s", 100, Some(TransferPurpose::Keep));
        book.sent("s", 0, 10);
        // The friend stopped this read (seeked, cancelled): not a failure.
        assert_eq!(book.end("s", false, false), Change::Progress);
        book.begin("s", 100, Some(TransferPurpose::Keep));
        assert_eq!(book.end("s", false, true), Change::Interrupted);
        assert_eq!(book.view("s").unwrap().state, TransferState::Interrupted);
        // Reconnecting clears it.
        book.begin("s", 100, None);
        assert_eq!(book.view("s").unwrap().state, TransferState::Active);
        assert_eq!(book.view("s").unwrap().purpose, TransferPurpose::Keep);
    }

    #[test]
    fn progress_notices_are_rate_limited() {
        let book = TransferBook::default();
        book.begin("s", 1_000, None);
        assert_eq!(book.sent("s", 0, 1), Some(Change::Progress));
        assert_eq!(book.sent("s", 1, 2), None);
        assert!(book.sent("other", 0, 1).is_none(), "unknown shares are ignored");
    }
}
