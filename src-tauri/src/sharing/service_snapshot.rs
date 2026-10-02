//! The renderer-facing snapshot: persisted state, live presence, stream
//! arrival and what friends have read of our shares.
use super::super::{
    invite::invite_link,
    model::{
        ClipOffer, FriendStatus, FriendView, InviteView, NodeStatus, Presence, ShareAnswer,
        SharedClipView, SharingSnapshot, TransferPurpose, TransferState, TransferView,
    },
    node::friend_code,
};
use super::SharingService;

impl SharingService {
    pub fn snapshot(&self) -> SharingSnapshot {
        let (status, status_message) = self.status();
        let friend_code = self.identity.get().map(|key| friend_code(&key.public()));
        let downloads = self.lock_downloads().clone();
        let pending_invite = self
            .pending_invite
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone();
        let window = self.share_window_ms.load(std::sync::atomic::Ordering::Relaxed);
        let mut snapshot = self.core.read(|state| {
            let name_of = |id: &str| {
                state
                    .friend(id)
                    .map(|friend| friend.name.clone())
                    .unwrap_or_else(|| "Former friend".into())
            };
            let mut inbox: Vec<_> = state
                .inbox
                .iter()
                .map(|clip| {
                    let mut view = view(
                        &clip.offer,
                        &clip.friend_id,
                        name_of(&clip.friend_id),
                        clip.received_at_ms,
                        clip.saved_path.is_some(),
                        true,
                    );
                    if clip.awaiting_answer {
                        view.answer = ShareAnswer::Pending;
                    }
                    view.available_until_ms = clip.accepted_at_ms.map(|at| at + window);
                    view
                })
                .collect();
            let mut outbox: Vec<_> = state
                .outbox
                .iter()
                .map(|share| {
                    let mut view = view(
                        &share.offer,
                        &share.friend_id,
                        name_of(&share.friend_id),
                        share.offer.created_at_ms,
                        share.kept,
                        share.delivered,
                    );
                    view.answer = share.answer;
                    view.available_until_ms = (share.answer == ShareAnswer::Accepted)
                        .then(|| share.accepted_at_ms.unwrap_or(share.offer.created_at_ms) + window);
                    // Live counts are lost on restart; a finished one is kept.
                    view.transfer = share.received_whole.then(|| TransferView {
                        sent_bytes: share.offer.size,
                        total_bytes: share.offer.size,
                        purpose: TransferPurpose::Keep,
                        state: TransferState::Complete,
                        bytes_per_second: 0,
                    });
                    view
                })
                .collect();
            inbox.reverse();
            outbox.reverse();
            SharingSnapshot {
                supported: true,
                enabled: state.enabled,
                appear_offline: state.appear_offline,
                status,
                status_message,
                invite_link: friend_code
                    .as_deref()
                    .filter(|_| state.enabled)
                    .map(|code| invite_link(code, &state.display_name)),
                pending_invite: pending_invite.map(|invite| InviteView {
                    already_friends: state.is_accepted(&invite.code),
                    code: invite.code,
                    name: invite.name,
                }),
                friend_code: friend_code.filter(|_| state.enabled),
                display_name: state.display_name.clone(),
                friends: state
                    .friends
                    .iter()
                    .map(|friend| FriendView {
                        friend: friend.clone(),
                        presence: if friend.status != FriendStatus::Accepted {
                            Presence::Offline
                        } else if state.appear_offline || status != NodeStatus::Online {
                            Presence::Unknown
                        } else if self.presence.is_online(&friend.id) {
                            Presence::Online
                        } else {
                            Presence::Offline
                        },
                    })
                    .collect(),
                inbox,
                outbox,
                downloads,
            }
        });
        // Outside the state lock: stream sessions have their own.
        for clip in &mut snapshot.inbox {
            clip.streamed = self.streams.arrived(&clip.share_id);
            clip.all_audio_ready = self.streams.complete(&clip.share_id);
        }
        for clip in &mut snapshot.outbox {
            if let Some(live) = self.transfers.view(&clip.share_id) {
                clip.transfer = Some(live);
            }
        }
        snapshot
    }
}

fn view(
    offer: &ClipOffer,
    friend_id: &str,
    friend_name: String,
    shared_at_ms: u64,
    saved: bool,
    delivered: bool,
) -> SharedClipView {
    SharedClipView {
        share_id: offer.share_id.clone(),
        friend_id: friend_id.into(),
        friend_name,
        title: offer.title.clone(),
        size: offer.size,
        duration_seconds: offer.duration_seconds,
        resolution: offer.resolution.clone(),
        game_or_app: offer.game_or_app.clone(),
        created_at_ms: offer.created_at_ms,
        shared_at_ms,
        saved,
        delivered,
        audio_tracks: offer.audio_tracks.clone(),
        streamed: Vec::new(),
        all_audio_ready: false,
        answer: ShareAnswer::Accepted,
        transfer: None,
        available_until_ms: None,
    }
}
