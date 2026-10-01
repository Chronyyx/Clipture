//! Which friends are online right now. Runtime only, never persisted: a friend
//! is online after they announce themselves or answer us, and until they say
//! goodbye or stay silent past the heartbeat window (crash, lost network).
use std::{
    collections::HashMap,
    sync::Mutex,
    time::{Duration, Instant},
};

/// Friends heartbeat every `HEARTBEAT`; missing two in a row means offline.
pub const HEARTBEAT: Duration = Duration::from_secs(60);
const EXPIRY: Duration = Duration::from_secs(150);

#[derive(Default)]
struct Entries {
    seen: HashMap<String, Instant>,
    /// When each friend last said goodbye. Evidence gathered before that
    /// moment (an answer to a request already in flight) cannot revive them.
    left: HashMap<String, Instant>,
}

#[derive(Default)]
pub struct PresenceBook {
    entries: Mutex<Entries>,
}

impl PresenceBook {
    /// Fresh contact right now. Returns true when they just came online.
    #[cfg(test)]
    pub fn saw(&self, id: &str) -> bool {
        self.saw_since(id, Instant::now())
    }

    /// The friend answered a request we started at `evidence`. Ignored when
    /// they said goodbye after we started it.
    pub fn saw_since(&self, id: &str, evidence: Instant) -> bool {
        let mut entries = self.lock();
        if entries.left.get(id).is_some_and(|left| *left > evidence) {
            return false;
        }
        entries.left.remove(id);
        let now = Instant::now();
        let was_online = entries
            .seen
            .get(id)
            .is_some_and(|last| now.duration_since(*last) < EXPIRY);
        entries.seen.insert(id.into(), now);
        !was_online
    }

    /// The friend said goodbye or could not be reached. Returns true when
    /// they were considered online.
    pub fn gone(&self, id: &str) -> bool {
        let now = Instant::now();
        let mut entries = self.lock();
        entries.left.insert(id.into(), now);
        entries
            .seen
            .remove(id)
            .is_some_and(|last| now.duration_since(last) < EXPIRY)
    }

    /// A request we started at `evidence` failed. Ignored when the friend
    /// proved they were online after we started it.
    pub fn unreachable_since(&self, id: &str, evidence: Instant) -> bool {
        if self
            .lock()
            .seen
            .get(id)
            .is_some_and(|last| *last > evidence)
        {
            return false;
        }
        self.gone(id)
    }

    pub fn is_online(&self, id: &str) -> bool {
        self.lock()
            .seen
            .get(id)
            .is_some_and(|last| last.elapsed() < EXPIRY)
    }

    pub fn online_ids(&self) -> Vec<String> {
        let now = Instant::now();
        self.lock()
            .seen
            .iter()
            .filter(|(_, last)| now.duration_since(**last) < EXPIRY)
            .map(|(id, _)| id.clone())
            .collect()
    }

    /// Drops expired entries; returns true when anyone went offline.
    pub fn expire(&self) -> bool {
        let now = Instant::now();
        let mut entries = self.lock();
        let before = entries.seen.len();
        entries
            .seen
            .retain(|_, last| now.duration_since(*last) < EXPIRY);
        entries
            .left
            .retain(|_, left| now.duration_since(*left) < EXPIRY);
        entries.seen.len() != before
    }

    pub fn clear(&self) {
        *self.lock() = Entries::default();
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Entries> {
        self.entries
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn contact_goodbye_and_silence_drive_presence() {
        let book = PresenceBook::default();
        assert!(book.saw("a"), "first contact comes online");
        assert!(!book.saw("a"), "a heartbeat is not a new arrival");
        assert!(book.is_online("a"));
        assert!(book.gone("a"));
        assert!(!book.is_online("a"));

        let long_ago = Instant::now() - EXPIRY - Duration::from_secs(1);
        book.lock().seen.insert("b".into(), long_ago);
        assert!(!book.is_online("b"), "silent friends expire");
        assert!(book.expire());
        assert!(book.online_ids().is_empty());
    }

    #[test]
    fn an_answer_already_in_flight_cannot_undo_a_goodbye() {
        let book = PresenceBook::default();
        let request_started = Instant::now();
        book.saw("a");
        book.gone("a");
        assert!(
            !book.saw_since("a", request_started),
            "stale answer ignored"
        );
        assert!(!book.is_online("a"));
        assert!(book.saw("a"), "a new request from them means they are back");
        assert!(book.is_online("a"));
        assert!(
            !book.unreachable_since("a", request_started),
            "stale failure ignored"
        );
        assert!(book.is_online("a"));
    }
}
