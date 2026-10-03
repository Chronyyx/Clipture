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

use super::model::{
    ClipOffer, Friend, FriendStatus, InboxClip, OutgoingShare, PendingAnswer, ShareAnswer,
};

const STATE_VERSION: u32 = 1;
pub const MAX_FRIENDS: usize = 200;
/// Strangers can only ask to be friends; cap how many unanswered asks we keep.
pub const MAX_INCOMING_REQUESTS: usize = 32;
pub const MAX_INBOX: usize = 500;
pub const MAX_OUTBOX: usize = 500;
const MAX_BLOCKED: usize = 1_000;
const MAX_ANSWERS: usize = 500;
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
    /// Accept/decline decisions not yet delivered to the sender.
    #[serde(default)]
    pub answers: Vec<PendingAnswer>,
    /// Friends we removed who have not heard it yet (they were offline).
    #[serde(default)]
    pub goodbyes: Vec<String>,
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
            answers: Vec::new(),
            goodbyes: Vec::new(),
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

/// How long a friend may watch or start keeping a clip once they accept.
pub const SHARE_WINDOW_MS: u64 = 15 * 60 * 1000;

/// Whether a friend may read a share now.
#[derive(Debug, PartialEq, Eq)]
pub struct Admission {
    pub open: bool,
    /// An older Clipture that never answers: this first read is its yes.
    pub implicitly_accepted: bool,
}

/// What a friend's answer changed about a share we sent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AnswerChange {
    Accepted,
    Declined,
    /// They saved a verified copy; the share is now closed.
    Kept,
    /// They deleted it after accepting; the share is now closed.
    Removed,
}

#[derive(Debug, PartialEq, Eq)]
pub enum OfferOutcome {
    /// A clip we have not seen: it waits for the user to accept or decline.
    New,
    /// Already in the inbox. If it was accepted, the answer is sent again.
    Known,
    /// Not from an accepted friend, or the inbox is full of kept clips.
    Refused,
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
        self.goodbyes.retain(|goodbye| goodbye != id);
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
            nickname: None,
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
            nickname: None,
        });
        HelloOutcome::Requested
    }

    /// Sets or clears (`None`) our own name for someone on the list.
    pub fn set_nickname(&mut self, id: &str, nickname: Option<String>) -> AppResult<()> {
        let friend = self
            .friends
            .iter_mut()
            .find(|friend| friend.id == id)
            .ok_or_else(|| AppError::Path("that friend is no longer on your list".into()))?;
        friend.nickname = nickname;
        Ok(())
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
        // Anyone who had us (or was asked to) must hear that it is over,
        // however long they stay offline.
        let connected = self.friend(id).is_some_and(|friend| friend.status != FriendStatus::Incoming);
        if connected && !self.goodbyes.iter().any(|goodbye| goodbye == id) {
            if self.goodbyes.len() >= MAX_FRIENDS {
                self.goodbyes.remove(0);
            }
            self.goodbyes.push(id.into());
        }
        self.friends.retain(|friend| friend.id != id);
        self.outbox.retain(|share| share.friend_id != id);
        self.inbox
            .retain(|clip| clip.friend_id != id || clip.saved_path.is_some());
        self.answers.retain(|answer| answer.friend_id != id);
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
        self.answers.retain(|answer| answer.friend_id != id);
        self.friends.len() != before
    }

    /// An accepted friend told us we are not on their list: they removed us
    /// while we could not hear it. Requests and declines are left alone, so
    /// this never reveals a declined request.
    pub fn forget_unfriended(&mut self, id: &str) -> bool {
        self.is_accepted(id) && self.forget_peer(id)
    }

    pub fn receive_offer(&mut self, from: &str, offer: ClipOffer, now_ms: u64) -> OfferOutcome {
        if !self.is_accepted(from) {
            return OfferOutcome::Refused;
        }
        if let Some(clip) = self.inbox_clip(&offer.share_id) {
            if clip.friend_id != from {
                // Share ids are random; reusing another friend's is hostile.
                return OfferOutcome::Refused;
            }
            // A re-send of a clip we already took: tell them again.
            if !clip.awaiting_answer {
                let kept = clip.saved_path.is_some();
                self.queue_answer(from, &offer.share_id, true, kept, false);
            }
            return OfferOutcome::Known;
        }
        if self.inbox.len() >= MAX_INBOX {
            // Evict the oldest clip that was never kept.
            match self.inbox.iter().position(|clip| clip.saved_path.is_none()) {
                Some(index) => {
                    self.inbox.remove(index);
                }
                None => return OfferOutcome::Refused,
            }
        }
        self.inbox.push(InboxClip {
            offer,
            friend_id: from.into(),
            received_at_ms: now_ms,
            saved_path: None,
            awaiting_answer: true,
            accepted_at_ms: None,
        });
        OfferOutcome::New
    }

    /// The user accepted or declined a friend's clip. A declined clip leaves
    /// the inbox; either way the friend is told. Returns the sender's id.
    pub fn answer_offer(&mut self, share_id: &str, accept: bool, now_ms: u64) -> AppResult<String> {
        let index = self
            .inbox
            .iter()
            .position(|clip| clip.offer.share_id == share_id && clip.awaiting_answer)
            .ok_or_else(|| AppError::Path("that clip is no longer waiting for an answer".into()))?;
        let friend_id = self.inbox[index].friend_id.clone();
        if accept {
            self.inbox[index].awaiting_answer = false;
            self.inbox[index].accepted_at_ms = Some(now_ms);
        } else {
            self.inbox.remove(index);
        }
        self.queue_answer(&friend_id, share_id, accept, false, false);
        Ok(friend_id)
    }

    /// Deletes a clip from the inbox. One that was accepted but never kept
    /// is still open on the sender's side, so they are told it was removed.
    /// Returns the sender's id when they need telling.
    pub fn remove_clip(&mut self, share_id: &str) -> Option<String> {
        let index = self.inbox.iter().position(|clip| clip.offer.share_id == share_id)?;
        let clip = self.inbox.remove(index);
        if clip.awaiting_answer || clip.saved_path.is_some() {
            return None;
        }
        self.queue_answer(&clip.friend_id, share_id, false, false, true);
        Some(clip.friend_id)
    }

    /// A verified copy of a friend's clip reached the library: tell them,
    /// so they stop serving it. Returns the sender's id.
    pub fn confirm_kept(&mut self, share_id: &str) -> Option<String> {
        let friend_id = self
            .inbox_clip(share_id)
            .filter(|clip| clip.saved_path.is_some())?
            .friend_id
            .clone();
        self.queue_answer(&friend_id, share_id, true, true, false);
        Some(friend_id)
    }

    fn queue_answer(&mut self, friend_id: &str, share_id: &str, accepted: bool, kept: bool, removed: bool) {
        self.answers.retain(|answer| answer.share_id != share_id);
        if self.answers.len() >= MAX_ANSWERS {
            self.answers.remove(0);
        }
        self.answers.push(PendingAnswer {
            friend_id: friend_id.into(),
            share_id: share_id.into(),
            accepted,
            kept,
            removed,
        });
    }

    /// A friend answered a clip we offered them; only that friend can.
    /// Returns what changed, if anything.
    pub fn receive_answer(
        &mut self,
        from: &str,
        share_id: &str,
        accepted: bool,
        kept: bool,
        removed: bool,
        now_ms: u64,
    ) -> Option<AnswerChange> {
        if !self.is_accepted(from) {
            return None;
        }
        let share = self
            .outbox
            .iter_mut()
            .find(|share| share.offer.share_id == share_id && share.friend_id == from)?;
        if accepted && kept {
            if share.kept {
                return None;
            }
            share.answer = ShareAnswer::Accepted;
            share.kept = true;
            return Some(AnswerChange::Kept);
        }
        let answer = if accepted {
            ShareAnswer::Accepted
        } else {
            ShareAnswer::Declined
        };
        if share.kept || (share.answer == answer && share.removed == removed) {
            return None;
        }
        share.answer = answer;
        if accepted {
            share.accepted_at_ms = Some(now_ms);
        }
        if removed && !accepted {
            share.removed = true;
            return Some(AnswerChange::Removed);
        }
        Some(if accepted { AnswerChange::Accepted } else { AnswerChange::Declined })
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
        self.outbox.iter().find(|share| {
            share.offer.share_id == share_id
                && share.friend_id == friend_id
                && share.answer != ShareAnswer::Declined
                && !share.kept
        })
    }

    /// Decides whether `friend_id` may start a read of `share_id` now. The
    /// window opens when they accept and lasts `window_ms`; a download that
    /// began inside it may finish (and resume) after it. Watching may not.
    pub fn admit_read(
        &mut self,
        friend_id: &str,
        share_id: &str,
        keep: bool,
        now_ms: u64,
        window_ms: u64,
    ) -> (Admission, bool) {
        let closed = Admission {
            open: false,
            implicitly_accepted: false,
        };
        if self.outgoing_for(friend_id, share_id).is_none() {
            return (closed, false);
        }
        let Some(share) = self.outbox.iter_mut().find(|share| share.offer.share_id == share_id) else {
            return (closed, false);
        };
        let mut changed = false;
        let implicitly_accepted = share.answer == ShareAnswer::Pending;
        if implicitly_accepted {
            share.answer = ShareAnswer::Accepted;
            share.accepted_at_ms = Some(now_ms);
            changed = true;
        }
        // Shares from before the window existed count from when they were sent.
        let opened = share.accepted_at_ms.unwrap_or(share.offer.created_at_ms);
        let within = now_ms < opened.saturating_add(window_ms);
        if within && keep && !share.keep_started {
            share.keep_started = true;
            changed = true;
        }
        let open = within || (keep && share.keep_started);
        (Admission { open, implicitly_accepted }, changed)
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
        assert_eq!(state.receive_offer("x", offer("s1"), 2), OfferOutcome::Refused);
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
            answer: ShareAnswer::Pending,
            received_whole: false,
            kept: false,
            accepted_at_ms: None,
            keep_started: false,
            linear: false,
            removed: false,
        });
        assert!(state.outgoing_for("a", "s1").is_some());
        assert!(state.outgoing_for("b", "s1").is_none());
        state.remove_friend("a");
        assert!(state.outgoing_for("a", "s1").is_none());
    }

    #[test]
    fn offers_wait_for_an_answer_that_is_queued_for_the_sender() {
        let mut state = StoredState::default();
        state.add_friend("a", "Alex", 1).unwrap();
        state.receive_hello("a", "Alex", 2);
        assert_eq!(state.receive_offer("a", offer("s1"), 3), OfferOutcome::New);
        assert!(state.inbox[0].awaiting_answer);
        assert_eq!(state.receive_offer("a", offer("s1"), 4), OfferOutcome::Known);
        assert!(state.answers.is_empty(), "no answer until the user decides");

        assert_eq!(state.answer_offer("s1", true, 5).unwrap(), "a");
        assert_eq!(state.inbox[0].accepted_at_ms, Some(5));
        assert!(!state.inbox[0].awaiting_answer);
        assert_eq!(state.answers.len(), 1);
        assert!(state.answer_offer("s1", false, 6).is_err(), "already answered");

        // A re-send of an accepted clip is answered again, once.
        state.answers.clear();
        state.receive_offer("a", offer("s1"), 5);
        state.receive_offer("a", offer("s1"), 6);
        assert_eq!(state.answers.len(), 1);

        state.receive_offer("a", offer("s2"), 7);
        state.answer_offer("s2", false, 8).unwrap();
        assert!(state.inbox_clip("s2").is_none(), "declined clips leave the inbox");
        assert!(state.answers.iter().any(|answer| answer.share_id == "s2" && !answer.accepted));
    }

    #[test]
    fn declined_shares_stop_serving_and_only_the_recipient_answers() {
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
            answer: ShareAnswer::Pending,
            received_whole: false,
            kept: false,
            accepted_at_ms: None,
            keep_started: false,
            linear: false,
            removed: false,
        });
        assert_eq!(state.receive_answer("b", "s1", false, false, false, 1), None);
        assert_eq!(state.receive_answer("a", "s1", false, false, false, 1), Some(AnswerChange::Declined));
        assert_eq!(state.receive_answer("a", "s1", false, false, false, 1), None);
        assert!(state.outgoing_for("a", "s1").is_none());

        // Kept: closed for good, and a stale answer cannot reopen it.
        state.outbox[0].answer = ShareAnswer::Pending;
        assert_eq!(state.receive_answer("a", "s1", true, false, false, 1), Some(AnswerChange::Accepted));
        assert!(state.outgoing_for("a", "s1").is_some());
        assert_eq!(state.receive_answer("a", "s1", true, true, false, 1), Some(AnswerChange::Kept));
        assert!(state.outgoing_for("a", "s1").is_none(), "they have it; it is not served again");
        assert_eq!(state.receive_answer("a", "s1", true, false, false, 1), None);
        assert!(state.outgoing_for("a", "s1").is_none());
    }

    #[test]
    fn deleting_an_accepted_clip_tells_the_sender_who_stops_serving_it() {
        // Recipient: deleting an accepted, unkept clip queues "removed".
        let mut state = StoredState::default();
        state.add_friend("a", "a", 1).unwrap();
        state.receive_hello("a", "a", 2);
        state.receive_offer("a", offer("s1"), 3);
        assert_eq!(state.remove_clip("s1"), None, "an unanswered clip is declined instead");
        state.receive_offer("a", offer("s2"), 4);
        state.answer_offer("s2", true, 5).unwrap();
        state.answers.clear();
        assert_eq!(state.remove_clip("s2").as_deref(), Some("a"));
        assert!(state.inbox_clip("s2").is_none());
        assert!(matches!(&state.answers[..], [answer] if answer.removed && !answer.accepted));

        // Sender: the share closes and shows as removed until sent again.
        let mut sender = StoredState::default();
        sender.add_friend("a", "a", 1).unwrap();
        sender.receive_hello("a", "a", 2);
        sender.add_outgoing(OutgoingShare {
            offer: offer("s2"),
            friend_id: "a".into(),
            path: PathBuf::from("C:\\Fixture\\clip.mp4"),
            delivered: true,
            source: None,
            source_blake3: None,
            answer: ShareAnswer::Accepted,
            received_whole: false,
            kept: false,
            accepted_at_ms: Some(5),
            keep_started: false,
            linear: false,
            removed: false,
        });
        assert_eq!(sender.receive_answer("a", "s2", false, false, true, 6), Some(AnswerChange::Removed));
        assert!(sender.outbox[0].removed);
        assert!(sender.outgoing_for("a", "s2").is_none());
        assert_eq!(sender.receive_answer("a", "s2", false, false, true, 7), None);
    }

    #[test]
    fn reads_close_after_the_window_but_a_started_download_may_finish() {
        let mut state = StoredState::default();
        state.add_friend("a", "a", 1).unwrap();
        state.receive_hello("a", "a", 2);
        let share = |id: &str| OutgoingShare {
            offer: offer(id),
            friend_id: "a".into(),
            path: PathBuf::from("C:\\Fixture\\clip.mp4"),
            delivered: true,
            source: None,
            source_blake3: None,
            answer: ShareAnswer::Pending,
            received_whole: false,
            kept: false,
            accepted_at_ms: None,
            keep_started: false,
            linear: false,
            removed: false,
        };
        state.add_outgoing(share("s1"));
        state.add_outgoing(share("s2"));
        state.receive_answer("a", "s1", true, false, false, 1_000);
        state.receive_answer("a", "s2", true, false, false, 1_000);
        let read = |state: &mut StoredState, id: &str, keep: bool, now: u64| state.admit_read("a", id, keep, now, 100).0.open;

        assert!(read(&mut state, "s1", false, 1_099), "inside the window");
        assert!(read(&mut state, "s2", true, 1_050), "a download starts inside it");
        assert!(!read(&mut state, "s1", false, 1_100), "watching ends with the window");
        assert!(!read(&mut state, "s1", true, 1_200), "too late to start keeping");
        assert!(read(&mut state, "s2", true, 5_000), "the started download may finish");
        assert!(!read(&mut state, "s2", false, 5_000), "but not be watched");

        // An older friend that never answers: the first read accepts and opens the window.
        state.add_outgoing(share("s3"));
        let (admission, changed) = state.admit_read("a", "s3", false, 9_000, 100);
        assert!(admission.open && admission.implicitly_accepted && changed);
        assert!(!read(&mut state, "s3", false, 9_100));
    }

    #[test]
    fn records_from_before_answers_read_as_accepted() {
        let offer = r#""offer":{"shareId":"s","title":"t","size":1,"blake3":"","durationSeconds":1,"resolution":"","gameOrApp":"","createdAtMs":1}"#;
        let share: OutgoingShare =
            serde_json::from_str(&format!(r#"{{{offer},"friendId":"a","path":"x","delivered":true}}"#)).unwrap();
        assert_eq!(share.answer, ShareAnswer::Accepted);
        let clip: InboxClip =
            serde_json::from_str(&format!(r#"{{{offer},"friendId":"a","receivedAtMs":1}}"#)).unwrap();
        assert!(!clip.awaiting_answer);
    }

    #[test]
    fn removals_are_remembered_until_they_reach_the_friend() {
        let mut state = StoredState::default();
        state.add_friend("a", "Alex", 1).unwrap();
        state.receive_hello("a", "Alex", 2);
        state.receive_hello("x", "X", 3);
        state.remove_friend("a");
        state.remove_friend("x");
        assert_eq!(state.goodbyes, ["a"], "declined requests need no goodbye");
        // Adding them again: nothing more to say.
        state.add_friend("a", "Alex", 4).unwrap();
        assert!(state.goodbyes.is_empty());

        let mut other = StoredState::default();
        other.add_friend("b", "Bo", 1).unwrap();
        assert!(!other.forget_unfriended("b"), "an unanswered request stays");
        other.receive_hello("b", "Bo", 2);
        assert!(other.forget_unfriended("b"));
        assert!(other.friends.is_empty());
        assert!(other.goodbyes.is_empty(), "being dropped is not a removal to announce");
    }

    #[test]
    fn a_nickname_outlasts_the_name_they_choose() {
        let mut state = StoredState::default();
        state.receive_hello("x", "Rude name", 1);
        state.set_nickname("x", Some("Sam".into())).unwrap();
        assert_eq!(state.friend("x").unwrap().display_name(), "Sam");
        // They rename themselves (still a request, so their name updates).
        state.receive_hello("x", "Ruder name", 2);
        assert_eq!(state.friend("x").unwrap().display_name(), "Sam");
        state.set_nickname("x", None).unwrap();
        assert_eq!(state.friend("x").unwrap().display_name(), "Ruder name");
        assert!(state.set_nickname("nobody", Some("A".into())).is_err());
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
