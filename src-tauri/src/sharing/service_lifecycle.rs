//! Node lifecycle, presence and delivery. When visible, Clipture announces
//! itself to every friend on start, heartbeats online friends, says goodbye
//! on the way out, and delivers waiting requests and clips the moment a
//! friend is seen. While appearing offline it sends nothing on its own.
use std::{
    sync::{atomic::Ordering, Arc},
    time::Duration,
};

use iroh::SecretKey;
use tokio::{sync::Semaphore, task::JoinSet, time::timeout};

use crate::error::{AppError, AppResult};

use super::super::{
    model::{ClipOffer, FriendStatus, NodeStatus, PendingAnswer},
    node::{is_not_friends, load_or_create_identity, parse_friend_code, Network, Node},
    presence::HEARTBEAT,
    server::PeerContext,
    wire::Request,
};
use super::SharingService;

/// Bounds the fan-out when announcing to many friends at once.
const ANNOUNCE_CONCURRENCY: usize = 16;
/// A goodbye must never hold up closing Clipture.
const GOODBYE_BUDGET: Duration = Duration::from_millis(1500);

impl SharingService {
    pub(super) fn start(self: &Arc<Self>) {
        if self.node().is_some() || self.status().0 == NodeStatus::Starting {
            return;
        }
        if matches!(self.network, Network::Disabled) {
            self.set_status(
                NodeStatus::Error,
                Some("Sharing is disabled in test mode.".into()),
            );
            return;
        }
        let secret = match self.identity() {
            Ok(secret) => secret,
            Err(error) => {
                self.set_status(NodeStatus::Error, Some(error.to_string()));
                return;
            }
        };
        let visible = !self.core.read(|state| state.appear_offline);
        self.presence.clear();
        self.set_status(NodeStatus::Starting, None);
        let generation = self.generation.fetch_add(1, Ordering::AcqRel) + 1;
        let weak = Arc::downgrade(self);
        let context = Arc::new(PeerContext {
            core: self.core.clone(),
            presence: self.presence.clone(),
            transfers: self.transfers.clone(),
            on_arrival: Box::new({
                let weak = weak.clone();
                move |friend_id| {
                    if let Some(this) = weak.upgrade() {
                        this.deliver_to_soon(friend_id);
                    }
                }
            }),
            share_window_ms: self.share_window_ms.clone(),
            on_closed: Box::new(move || {
                if let Some(this) = weak.upgrade() {
                    this.tidy_copies();
                }
            }),
        });
        let this = self.clone();
        self.runtime.spawn(async move {
            let result = Node::start(context, secret, this.network.clone(), visible).await;
            if this.generation.load(Ordering::Acquire) != generation {
                if let Ok(node) = result {
                    node.shutdown().await;
                }
                return;
            }
            let node = match result {
                Ok(node) => Arc::new(node),
                Err(error) => {
                    this.set_status(NodeStatus::Error, Some(error.to_string()));
                    return;
                }
            };
            *this
                .node
                .write()
                .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(node.clone());
            this.set_status(NodeStatus::Online, None);
            if !visible {
                return;
            }
            // Everyone who answers is online and gets their deliveries.
            this.announce(&node, this.accepted_friends(), true).await;
            // Anything queued while the node was starting (or since last run).
            this.deliver_all(&node).await;
            while this.generation.load(Ordering::Acquire) == generation {
                tokio::time::sleep(HEARTBEAT).await;
                if this.generation.load(Ordering::Acquire) != generation {
                    break;
                }
                this.announce(&node, this.presence.online_ids(), true).await;
                if this.presence.expire() {
                    this.core.events.changed();
                }
                // Requests to people who have not added us yet, and goodbyes
                // to people we removed, cannot rely on presence; they are
                // retried on the heartbeat.
                this.deliver_requests(&node).await;
                this.deliver_goodbyes(&node).await;
            }
        });
    }

    /// Stops the node in the background, saying goodbye first when visible.
    pub(super) fn stop(&self) {
        if let Some((node, friends)) = self.detach() {
            self.runtime.spawn(async move {
                let _ = timeout(GOODBYE_BUDGET, say_goodbye(&node, friends)).await;
                node.shutdown().await;
            });
        }
    }

    /// Application exit: waits briefly so friends see us leave right away.
    pub fn shutdown(&self) {
        let Some((node, friends)) = self.detach() else {
            return;
        };
        let goodbye = async move {
            let _ = timeout(GOODBYE_BUDGET, say_goodbye(&node, friends)).await;
            node.shutdown().await;
        };
        if tokio::runtime::Handle::try_current().is_ok() {
            self.runtime.spawn(goodbye);
        } else {
            self.runtime.block_on(goodbye);
        }
    }

    /// Takes the node out of service; returns who to tell we left.
    fn detach(&self) -> Option<(Arc<Node>, Vec<String>)> {
        self.generation.fetch_add(1, Ordering::AcqRel);
        self.streams.clear();
        let node = self
            .node
            .write()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .take();
        let friends = self.presence.online_ids();
        self.presence.clear();
        self.set_status(NodeStatus::Off, None);
        node.map(|node| (node, friends))
    }

    fn visible(&self) -> bool {
        !self.core.read(|state| state.appear_offline)
    }

    fn accepted_friends(&self) -> Vec<String> {
        self.core.read(|state| {
            state
                .friends
                .iter()
                .filter(|friend| friend.status == FriendStatus::Accepted)
                .map(|friend| friend.id.clone())
                .collect()
        })
    }

    /// Presence to many friends at once. The node records who answered
    /// (online, which triggers their deliveries) and who did not (offline).
    /// A friend who answers that we are not on their list removed us while
    /// we could not hear it, so they are dropped here too.
    async fn announce(&self, node: &Arc<Node>, friends: Vec<String>, online: bool) {
        let gate = Arc::new(Semaphore::new(ANNOUNCE_CONCURRENCY));
        let mut tasks = JoinSet::new();
        for id in friends {
            let Ok(peer) = parse_friend_code(&id) else {
                continue;
            };
            let node = node.clone();
            let gate = gate.clone();
            tasks.spawn(async move {
                let _permit = gate.acquire_owned().await;
                let result = node.notify(peer, &Request::Presence { online }).await;
                (id, result)
            });
        }
        while let Some(joined) = tasks.join_next().await {
            if let Ok((id, Err(error))) = joined {
                self.forget_if_unfriended(&id, &error);
            }
        }
    }

    /// Drops an accepted friend whose Clipture says they removed us.
    fn forget_if_unfriended(&self, friend_id: &str, error: &AppError) -> bool {
        if !is_not_friends(error) {
            return false;
        }
        let forgotten = self.core.update_when(|state| {
            let forgotten = state.forget_unfriended(friend_id);
            (forgotten, forgotten)
        });
        if forgotten.unwrap_or(false) {
            self.presence.gone(friend_id);
            self.tidy_copies();
        }
        true
    }

    /// Tells one removed friend; done once they hear it or say they already
    /// removed us. Unreachable friends keep it for the next try.
    pub(super) async fn send_goodbye(&self, node: &Node, friend_id: &str) {
        let Ok(peer) = parse_friend_code(friend_id) else {
            return;
        };
        let result = node.notify(peer, &Request::Goodbye {}).await;
        if result.is_ok() || result.as_ref().is_err_and(is_not_friends) {
            let _ = self.core.update_when(|state| {
                let before = state.goodbyes.len();
                state.goodbyes.retain(|goodbye| goodbye != friend_id);
                ((), state.goodbyes.len() != before)
            });
        }
    }

    async fn deliver_goodbyes(&self, node: &Node) {
        let owed = self.core.read(|state| state.goodbyes.clone());
        for id in owed {
            self.send_goodbye(node, &id).await;
        }
    }

    /// Something new to send (a request, acceptance or clip). Sends now to
    /// anyone reachable; the rest get it when they next come online.
    pub(super) fn deliver_soon(self: &Arc<Self>) {
        let Some(node) = self.node().filter(|_| self.visible()) else {
            // Not up yet: `start` delivers everything once the node is online.
            return;
        };
        let this = self.clone();
        self.runtime
            .spawn(async move { this.deliver_all(&node).await });
    }

    /// Sends every waiting request, acceptance and clip. Duplicates are
    /// harmless: peers ignore a repeated hello or an offer they already have.
    async fn deliver_all(&self, node: &Node) {
        self.deliver_requests(node).await;
        self.deliver_goodbyes(node).await;
        // Try every accepted friend with something waiting, even one we
        // have not heard from: they may have come online meanwhile.
        let pending = self.core.read(|state| {
            let mut ids: Vec<String> = state
                .friends
                .iter()
                .filter(|friend| friend.undelivered && friend.status == FriendStatus::Accepted)
                .map(|friend| friend.id.clone())
                .chain(
                    state
                        .outbox
                        .iter()
                        .filter(|share| !share.delivered)
                        .map(|share| share.friend_id.clone()),
                )
                .chain(state.answers.iter().map(|answer| answer.friend_id.clone()))
                .collect();
            ids.sort();
            ids.dedup();
            ids
        });
        for friend in pending {
            self.deliver_to(node, &friend).await;
        }
    }

    pub(super) fn deliver_to_soon(self: &Arc<Self>, friend_id: String) {
        let Some(node) = self.node().filter(|_| self.visible()) else {
            return;
        };
        let this = self.clone();
        self.runtime
            .spawn(async move { this.deliver_to(&node, &friend_id).await });
    }

    /// Friend requests to people who have not accepted yet.
    async fn deliver_requests(&self, node: &Node) {
        let (name, requests) = self.core.read(|state| {
            let requests: Vec<String> = state
                .friends
                .iter()
                .filter(|friend| friend.undelivered && friend.status == FriendStatus::Outgoing)
                .map(|friend| friend.id.clone())
                .collect();
            (state.display_name.clone(), requests)
        });
        for id in requests {
            self.send_hello(node, &id, &name).await;
        }
    }

    /// Everything waiting for one accepted friend: our acceptance, clips,
    /// and our answers to clips they sent.
    async fn deliver_to(&self, node: &Node, friend_id: &str) {
        let (name, hello, offers, answers) = self.core.read(|state| {
            let hello = state.friend(friend_id).is_some_and(|friend| {
                friend.undelivered && friend.status != FriendStatus::Incoming
            });
            let offers: Vec<ClipOffer> = state
                .outbox
                .iter()
                .filter(|share| share.friend_id == friend_id && !share.delivered)
                .filter(|_| state.is_accepted(friend_id))
                .map(|share| share.offer.clone())
                .collect();
            let answers: Vec<PendingAnswer> = state
                .answers
                .iter()
                .filter(|answer| answer.friend_id == friend_id)
                .cloned()
                .collect();
            (state.display_name.clone(), hello, offers, answers)
        });
        if hello {
            self.send_hello(node, friend_id, &name).await;
        }
        let Ok(peer) = parse_friend_code(friend_id) else {
            return;
        };
        for offer in offers {
            let share_id = offer.share_id.clone();
            if let Err(error) = node.notify(peer, &Request::Offer { offer }).await {
                self.forget_if_unfriended(friend_id, &error);
                return;
            }
            let _ = self.core.update(|state| {
                if let Some(share) = state
                    .outbox
                    .iter_mut()
                    .find(|share| share.offer.share_id == share_id)
                {
                    share.delivered = true;
                }
            });
        }
        for answer in answers {
            let request = Request::Answer {
                share_id: answer.share_id.clone(),
                accepted: answer.accepted,
                kept: answer.kept,
            };
            // A failed request marks them offline, so ask first.
            let was_online = self.presence.is_online(friend_id);
            if let Err(error) = node.notify(peer, &request).await {
                // Unreachable: try again when they are next seen. Online but
                // refused (an older Clipture rejects the message type):
                // retrying cannot help, so the answer is dropped.
                if !was_online {
                    return;
                }
                tracing::debug!(%error, "friend did not take our answer");
            }
            let _ = self.core.update(|state| state.answers.retain(|queued| queued != &answer));
        }
    }

    async fn send_hello(&self, node: &Node, id: &str, name: &str) {
        let Ok(peer) = parse_friend_code(id) else {
            return;
        };
        let hello = Request::Hello { name: name.into() };
        let result = node.notify(peer, &hello).await;
        if let Err(error) = &result {
            self.forget_if_unfriended(id, error);
        }
        if result.is_ok() {
            let _ = self.core.update(|state| {
                if let Some(friend) = state.friends.iter_mut().find(|friend| friend.id == id) {
                    friend.undelivered = false;
                }
            });
        }
    }

    pub(super) fn identity(&self) -> AppResult<SecretKey> {
        if let Some(secret) = self.identity.get() {
            return Ok(secret.clone());
        }
        let secret = load_or_create_identity(self.core.file().directory())?;
        Ok(self.identity.get_or_init(|| secret).clone())
    }

    pub(super) fn node(&self) -> Option<Arc<Node>> {
        self.node
            .read()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone()
    }

    pub(super) fn require_node(&self) -> AppResult<Arc<Node>> {
        self.node()
            .ok_or_else(|| AppError::Path("turn on friend sharing first".into()))
    }

    pub(super) fn status(&self) -> (NodeStatus, Option<String>) {
        self.status
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone()
    }

    pub(super) fn set_status(&self, status: NodeStatus, message: Option<String>) {
        *self
            .status
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = (status, message);
        self.core.events.changed();
    }
}

async fn say_goodbye(node: &Arc<Node>, friends: Vec<String>) {
    let mut tasks = JoinSet::new();
    for id in friends {
        let Ok(peer) = parse_friend_code(&id) else {
            continue;
        };
        let node = node.clone();
        tasks.spawn(async move {
            let _ = node
                .notify(peer, &Request::Presence { online: false })
                .await;
        });
    }
    while tasks.join_next().await.is_some() {}
}
