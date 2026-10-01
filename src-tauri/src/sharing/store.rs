//! Persistent sharing state under `%APPDATA%\Clipture\data\sharing`. All
//! friend-list policy lives here as plain, testable state transitions.
use std::{
    fs,
    io::{BufWriter, Write},
    path::{Path, PathBuf},
};

use serde::{Deserialize, Serialize};
use tempfile::NamedTempFile;

use crate::error::{AppError, AppResult};

use super::model::{ClipOffer, Friend, FriendStatus, InboxClip, OutgoingShare};

const STATE_VERSION: u32 = 1;
pub const MAX_FRIENDS: usize = 200;
/// Strangers can only ask to be friends; cap how many unanswered asks we keep.
pub const MAX_INCOMING_REQUESTS: usize = 32;
pub const MAX_INBOX: usize = 500;
pub const MAX_OUTBOX: usize = 500;
const MAX_BLOCKED: usize = 1_000;
pub const MAX_NAME_CHARS: usize = 40;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StoredState {
    pub version: u32,
    pub enabled: bool,
    /// Refuse all incoming connections and stop announcing presence.
    #[serde(default)]
    pub appear_offline: bool,
    pub display_name: String,
    pub friends: Vec<Friend>,
    /// Peers whose requests were declined; their requests are ignored.
    pub blocked: Vec<String>,
    pub outbox: Vec<OutgoingShare>,
    pub inbox: Vec<InboxClip>,
}

impl Default for StoredState {
    fn default() -> Self {
        Self {
            version: STATE_VERSION,
            enabled: false,
            appear_offline: false,
            display_name: "Clipture friend".into(),
            friends: Vec::new(),
            blocked: Vec::new(),
            outbox: Vec::new(),
            inbox: Vec::new(),
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum HelloOutcome {
    /// The peer is (now) an accepted friend.
    Friends,
    /// Recorded as an incoming request awaiting the user's answer.
    Requested,
    /// Blocked, over quota, or otherwise dropped without a trace.
    Ignored,
}

impl StoredState {
    pub fn friend(&self, id: &str) -> Option<&Friend> {
        self.friends.iter().find(|friend| friend.id == id)
    }

    pub fn is_accepted(&self, id: &str) -> bool {
        self.friend(id)
            .is_some_and(|friend| friend.status == FriendStatus::Accepted)
    }

    /// The local user entered a friend code. Returns the resulting status.
    pub fn add_friend(&mut self, id: &str, name: &str, now_ms: u64) -> AppResult<FriendStatus> {
        self.blocked.retain(|blocked| blocked != id);
        if let Some(friend) = self.friends.iter_mut().find(|friend| friend.id == id) {
            if friend.status == FriendStatus::Incoming {
                // They already asked us: adding their code accepts it.
                friend.status = FriendStatus::Accepted;
            }
            if !name.is_empty() {
                friend.name = name.into();
            }
            friend.undelivered = true;
            return Ok(friend.status);
        }
        if self.friends.len() >= MAX_FRIENDS {
            return Err(AppError::Path("the friends list is full".into()));
        }
        self.friends.push(Friend {
            id: id.into(),
            name: if name.is_empty() {
                "Friend".into()
            } else {
                name.into()
            },
            status: FriendStatus::Outgoing,
            added_at_ms: now_ms,
            undelivered: true,
        });
        Ok(FriendStatus::Outgoing)
    }

    /// A remote peer (authenticated by its key) said hello.
    pub fn receive_hello(&mut self, id: &str, name: &str, now_ms: u64) -> HelloOutcome {
        if self.blocked.iter().any(|blocked| blocked == id) {
            return HelloOutcome::Ignored;
        }
        if let Some(friend) = self.friends.iter_mut().find(|friend| friend.id == id) {
            if !name.is_empty() && friend.status != FriendStatus::Accepted {
                friend.name = name.into();
            }
            return match friend.status {
                FriendStatus::Accepted => HelloOutcome::Friends,
                FriendStatus::Outgoing => {
                    // Both sides asked. Our earlier hello was answered but may
                    // have been ignored, so say hello again to finish the
                    // handshake on their side too.
                    friend.status = FriendStatus::Accepted;
                    friend.undelivered = true;
                    HelloOutcome::Friends
                }
                FriendStatus::Incoming => HelloOutcome::Requested,
            };
        }
        let pending = self
            .friends
            .iter()
            .filter(|friend| friend.status == FriendStatus::Incoming)
            .count();
        if pending >= MAX_INCOMING_REQUESTS || self.friends.len() >= MAX_FRIENDS {
            return HelloOutcome::Ignored;
        }
        self.friends.push(Friend {
            id: id.into(),
            name: if name.is_empty() {
                "Unknown".into()
            } else {
                name.into()
            },
            status: FriendStatus::Incoming,
            added_at_ms: now_ms,
            undelivered: false,
        });
        HelloOutcome::Requested
    }

    pub fn accept_request(&mut self, id: &str) -> AppResult<()> {
        let friend = self
            .friends
            .iter_mut()
            .find(|friend| friend.id == id && friend.status == FriendStatus::Incoming)
            .ok_or_else(|| AppError::Path("there is no pending request from that friend".into()))?;
        friend.status = FriendStatus::Accepted;
        friend.undelivered = true;
        Ok(())
    }

    /// Removes a friend or declines a request. Declined requests are blocked
    /// so a stranger cannot re-appear by asking again.
    pub fn remove_friend(&mut self, id: &str) -> bool {
        let before = self.friends.len();
        let declined = self
            .friends
            .iter()
            .any(|friend| friend.id == id && friend.status == FriendStatus::Incoming);
        self.friends.retain(|friend| friend.id != id);
        self.outbox.retain(|share| share.friend_id != id);
        self.inbox
            .retain(|clip| clip.friend_id != id || clip.saved_path.is_some());
        if declined && !self.blocked.iter().any(|blocked| blocked == id) {
            if self.blocked.len() >= MAX_BLOCKED {
                self.blocked.remove(0);
            }
            self.blocked.push(id.into());
        }
        self.friends.len() != before
    }

    /// The remote peer removed us; drop them without blocking.
    pub fn forget_peer(&mut self, id: &str) -> bool {
        let before = self.friends.len();
        self.friends.retain(|friend| friend.id != id);
        self.outbox.retain(|share| share.friend_id != id);
        self.inbox
            .retain(|clip| clip.friend_id != id || clip.saved_path.is_some());
        self.friends.len() != before
    }

    pub fn receive_offer(&mut self, from: &str, offer: ClipOffer, now_ms: u64) -> bool {
        if !self.is_accepted(from) {
            return false;
        }
        if self
            .inbox
            .iter()
            .any(|clip| clip.offer.share_id == offer.share_id)
        {
            return true;
        }
        if self.inbox.len() >= MAX_INBOX {
            // Evict the oldest clip that was never kept.
            match self.inbox.iter().position(|clip| clip.saved_path.is_none()) {
                Some(index) => {
                    self.inbox.remove(index);
                }
                None => return false,
            }
        }
        self.inbox.push(InboxClip {
            offer,
            friend_id: from.into(),
            received_at_ms: now_ms,
            saved_path: None,
        });
        true
    }

    pub fn add_outgoing(&mut self, share: OutgoingShare) {
        if self.outbox.len() >= MAX_OUTBOX {
            self.outbox.remove(0);
        }
        self.outbox.push(share);
    }

    /// Only the friend a clip was offered to may read it.
    pub fn outgoing_for(&self, friend_id: &str, share_id: &str) -> Option<&OutgoingShare> {
        if !self.is_accepted(friend_id) {
            return None;
        }
        self.outbox
            .iter()
            .find(|share| share.offer.share_id == share_id && share.friend_id == friend_id)
    }

    pub fn inbox_clip(&self, share_id: &str) -> Option<&InboxClip> {
        self.inbox
            .iter()
            .find(|clip| clip.offer.share_id == share_id)
    }
}

pub struct StateFile {
    directory: PathBuf,
    file: PathBuf,
}

impl StateFile {
    pub fn new(directory: PathBuf) -> Self {
        let file = directory.join("state.json");
        Self { directory, file }
    }

    pub fn directory(&self) -> &Path {
        &self.directory
    }

    /// A missing or unreadable file yields defaults; a corrupt file is kept
    /// aside instead of being silently overwritten.
    pub fn load(&self) -> StoredState {
        let Ok(bytes) = fs::read(&self.file) else {
            return StoredState::default();
        };
        match serde_json::from_slice::<StoredState>(&bytes) {
            Ok(state) if state.version == STATE_VERSION => state,
            _ => {
                let _ = fs::rename(&self.file, self.directory.join("state.corrupt.json"));
                tracing::warn!("sharing state was unreadable and has been reset");
                StoredState::default()
            }
        }
    }

    pub fn save(&self, state: &StoredState) -> AppResult<()> {
        fs::create_dir_all(&self.directory).map_err(|source| AppError::Io {
            action: "create sharing directory",
            path: self.directory.clone(),
            source,
        })?;
        let mut temporary =
            NamedTempFile::new_in(&self.directory).map_err(|source| AppError::Io {
                action: "create temporary sharing state",
                path: self.directory.clone(),
                source,
            })?;
        {
            let mut writer = BufWriter::new(temporary.as_file_mut());
            serde_json::to_writer_pretty(&mut writer, state)?;
            writer.flush().map_err(|source| AppError::Io {
                action: "write sharing state",
                path: self.file.clone(),
                source,
            })?;
        }
        temporary
            .persist(&self.file)
            .map_err(|error| AppError::Io {
                action: "replace sharing state",
                path: self.file.clone(),
                source: error.error,
            })?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn offer(id: &str) -> ClipOffer {
        ClipOffer {
            share_id: id.into(),
            title: "Clip".into(),
            size: 10,
            blake3: "00".repeat(32),
            duration_seconds: 30,
            resolution: "1920x1080".into(),
            game_or_app: "Game".into(),
            created_at_ms: 1,
            fps: 60,
            audio_tracks: Vec::new(),
        }
    }

    #[test]
    fn mutual_codes_become_friends_from_either_side() {
        let mut state = StoredState::default();
        assert_eq!(
            state.add_friend("a", "Alex", 1).unwrap(),
            FriendStatus::Outgoing
        );
        assert!(!state.is_accepted("a"));
        state.friends[0].undelivered = false;
        assert_eq!(state.receive_hello("a", "Alex", 2), HelloOutcome::Friends);
        assert!(state.is_accepted("a"));
        assert!(state.friend("a").unwrap().undelivered, "must answer with its own hello");

        let mut other = StoredState::default();
        assert_eq!(other.receive_hello("b", "Bo", 1), HelloOutcome::Requested);
        assert!(!other.is_accepted("b"));
        assert_eq!(
            other.add_friend("b", "", 2).unwrap(),
            FriendStatus::Accepted
        );
    }

    #[test]
    fn strangers_cannot_offer_and_are_capped() {
        let mut state = StoredState::default();
        assert_eq!(state.receive_hello("x", "X", 1), HelloOutcome::Requested);
        assert!(!state.receive_offer("x", offer("s1"), 2));
        for index in 0..MAX_INCOMING_REQUESTS {
            state.receive_hello(&format!("p{index}"), "P", 3);
        }
        assert_eq!(state.receive_hello("late", "L", 4), HelloOutcome::Ignored);
    }

    #[test]
    fn declined_requests_stay_blocked_until_added() {
        let mut state = StoredState::default();
        state.receive_hello("x", "X", 1);
        assert!(state.remove_friend("x"));
        assert_eq!(state.receive_hello("x", "X", 2), HelloOutcome::Ignored);
        state.add_friend("x", "X", 3).unwrap();
        assert_eq!(state.receive_hello("x", "X", 4), HelloOutcome::Friends);
    }

    #[test]
    fn a_share_is_readable_only_by_its_recipient() {
        let mut state = StoredState::default();
        for id in ["a", "b"] {
            state.add_friend(id, id, 1).unwrap();
            state.receive_hello(id, id, 2);
        }
        state.add_outgoing(OutgoingShare {
            offer: offer("s1"),
            friend_id: "a".into(),
            path: PathBuf::from("C:\\Fixture\\clip.mp4"),
            delivered: true,
            source: None,
            source_blake3: None,
        });
        assert!(state.outgoing_for("a", "s1").is_some());
        assert!(state.outgoing_for("b", "s1").is_none());
        state.remove_friend("a");
        assert!(state.outgoing_for("a", "s1").is_none());
    }

    #[test]
    fn state_round_trips_through_an_isolated_directory() {
        let directory = tempfile::tempdir().unwrap();
        let file = StateFile::new(directory.path().join("sharing"));
        let mut state = StoredState::default();
        state.add_friend("a", "Alex", 1).unwrap();
        file.save(&state).unwrap();
        assert_eq!(file.load().friends, state.friends);
    }
}
