//! Inbound request handling. Strangers may only say hello (a friend request);
//! everything else requires an accepted friend, and a clip can be read only
//! by the friend it was offered to. Local paths are never sent to peers.
//! While the user appears offline, every connection is refused before the
//! TLS handshake completes, so peers cannot tell this device is running.
use std::{
    fmt,
    sync::Arc,
    time::{Duration, Instant},
};

use iroh::{
    endpoint::{Accepting, Connection, RecvStream, SendStream},
    protocol::{AcceptError, ProtocolHandler},
};
use tokio::{
    io::{AsyncReadExt, AsyncSeekExt},
    sync::Semaphore,
    time::timeout,
};

use super::{
    core::{now_ms, Core},
    node::friend_code,
    presence::PresenceBook,
    store::HelloOutcome,
    wire::{self, Request, Response},
};

const MAX_CONNECTIONS: usize = 32;
const MAX_STREAMS_PER_FRIEND: usize = 8;
const REQUEST_READ_TIMEOUT: Duration = Duration::from_secs(10);
const IDLE_TIMEOUT: Duration = Duration::from_secs(120);
const SEND_BUFFER: usize = 1024 * 1024;

/// What request handling needs besides persisted state.
pub struct PeerContext {
    pub core: Arc<Core>,
    pub presence: Arc<PresenceBook>,
    /// Called when a friend comes online, to deliver anything waiting.
    pub on_arrival: Box<dyn Fn(String) + Send + Sync>,
}

impl PeerContext {
    /// Any request from an accepted friend proves they are online.
    pub(super) fn contact_if_friend(&self, peer: &str) {
        self.contact_if_friend_since(peer, Instant::now());
    }

    /// `evidence` is when the proof of life was requested: for our own
    /// requests, when we sent them, so an answer that raced a goodbye is
    /// not mistaken for the friend coming back.
    pub(super) fn contact_if_friend_since(&self, peer: &str, evidence: Instant) {
        if !self.core.read(|state| state.is_accepted(peer)) {
            return;
        }
        if self.presence.saw_since(peer, evidence) {
            self.core.events.changed();
            (self.on_arrival)(peer.to_owned());
        }
    }
}

#[derive(Clone)]
pub struct ShareProtocol {
    context: Arc<PeerContext>,
    visible: bool,
    connections: Arc<Semaphore>,
}

impl fmt::Debug for ShareProtocol {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("ShareProtocol")
    }
}

impl ShareProtocol {
    pub fn new(context: Arc<PeerContext>, visible: bool) -> Self {
        Self {
            context,
            visible,
            connections: Arc::new(Semaphore::new(MAX_CONNECTIONS)),
        }
    }
}

impl ProtocolHandler for ShareProtocol {
    async fn on_accepting(&self, accepting: Accepting) -> Result<Connection, AcceptError> {
        if !self.visible {
            return Err(AcceptError::from_err(std::io::Error::other(
                "appearing offline",
            )));
        }
        Ok(accepting.await?)
    }

    async fn accept(&self, connection: Connection) -> Result<(), AcceptError> {
        let Ok(_permit) = self.connections.clone().try_acquire_owned() else {
            connection.close(1u32.into(), b"busy");
            return Ok(());
        };
        let peer = friend_code(&connection.remote_id());
        let streams = Arc::new(Semaphore::new(MAX_STREAMS_PER_FRIEND));
        loop {
            let Ok(Ok((send, recv))) = timeout(IDLE_TIMEOUT, connection.accept_bi()).await else {
                break;
            };
            if !self.context.core.read(|state| state.is_accepted(&peer)) {
                // Anyone who is not a friend yet gets exactly one request.
                handle_stream(self.context.clone(), peer.clone(), send, recv).await;
                break;
            }
            let Ok(permit) = streams.clone().try_acquire_owned() else {
                // Dropping the streams resets them; the peer retries later.
                continue;
            };
            let context = self.context.clone();
            let peer = peer.clone();
            tokio::spawn(async move {
                let _permit = permit;
                handle_stream(context, peer, send, recv).await;
            });
        }
        connection.close(0u32.into(), b"idle");
        Ok(())
    }
}

async fn handle_stream(
    context: Arc<PeerContext>,
    peer: String,
    mut send: SendStream,
    mut recv: RecvStream,
) {
    let core = &context.core;
    let request = match timeout(
        REQUEST_READ_TIMEOUT,
        wire::read_message::<_, Request>(&mut recv),
    )
    .await
    {
        Ok(Ok(request)) => request,
        Ok(Err(error)) => {
            tracing::debug!(%error, "rejected malformed peer request");
            return;
        }
        Err(_) => return,
    };
    let result = match request {
        Request::Range {
            share_id,
            start,
            length,
            sparse,
        } => {
            context.contact_if_friend(&peer);
            serve_range(core, &peer, &share_id, start..start + length, sparse, &mut send).await
        }
        Request::Presence { online: false } => {
            if context.presence.gone(&peer) {
                core.events.changed();
            }
            reply(&mut send, Response::Ok {}).await
        }
        other => {
            let response = answer(core, &peer, other);
            // After answering, so a hello that completes a friendship counts.
            context.contact_if_friend(&peer);
            wire::write_message(&mut send, &response)
                .await
                .map_err(|error| error.to_string())
        }
    };
    match result {
        Ok(()) => {
            let _ = send.finish();
        }
        Err(error) => tracing::debug!(%error, "peer request ended early"),
    }
}

fn answer(core: &Core, peer: &str, request: Request) -> Response {
    let saved = match request {
        Request::Hello { name } => {
            let name = wire::clean_name(&name);
            core.update_when(|state| {
                let before = state.friends.clone();
                let outcome = state.receive_hello(peer, &name, now_ms());
                (outcome, state.friends != before)
            })
            .map(|outcome| outcome != HelloOutcome::Ignored)
        }
        Request::Offer { offer } => {
            if !core.read(|state| state.is_accepted(peer)) {
                return denied("you are not friends yet");
            }
            let Ok(offer) = wire::sanitize_offer(offer) else {
                return denied("the clip description was invalid");
            };
            core.update(|state| state.receive_offer(peer, offer, now_ms()))
        }
        // Only accepted friends' presence counts; contact is noted by the caller.
        Request::Presence { .. } => Ok(true),
        Request::Goodbye {} => core.update_when(|state| {
            let removed = state.forget_peer(peer);
            (removed, removed)
        }),
        Request::Range { .. } => unreachable!("ranges are streamed separately"),
    };
    match saved {
        // Ignored hellos still read as success so strangers learn nothing.
        Ok(_) => Response::Ok {},
        Err(error) => {
            tracing::warn!(%error, "could not record a peer request");
            denied("the request could not be saved")
        }
    }
}

async fn serve_range(
    core: &Core,
    peer: &str,
    share_id: &str,
    range: std::ops::Range<u64>,
    sparse: bool,
    send: &mut SendStream,
) -> Result<(), String> {
    let start = range.start;
    let length = range.end.saturating_sub(range.start);
    let share = core.read(|state| {
        state
            .outgoing_for(peer, share_id)
            .map(|share| (share.path.clone(), share.offer.size))
    });
    let Some((path, size)) = share else {
        return reply(send, denied("this clip is not shared with you")).await;
    };
    let in_bounds = length > 0 && start.checked_add(length).is_some_and(|end| end <= size);
    if !in_bounds {
        return reply(send, denied("the requested range is invalid")).await;
    }
    let file = async {
        let mut file = tokio::fs::File::open(&path).await?;
        // A clip that was edited after sharing no longer matches its digest.
        if file.metadata().await?.len() != size {
            return Err(std::io::Error::other("size changed"));
        }
        file.seek(std::io::SeekFrom::Start(start)).await?;
        Ok(file)
    };
    let file = match file.await {
        Ok(file) => file,
        Err(_) => return reply(send, denied("the clip was moved, edited or deleted")).await,
    };
    let total = size;
    let response = if sparse {
        Response::SparseRange { total, length }
    } else {
        Response::Range { total, length }
    };
    reply(send, response).await?;
    // Large reads keep the QUIC send buffer full; the default is 8 KiB.
    let source = tokio::io::BufReader::with_capacity(SEND_BUFFER, file.take(length));
    super::range_body::send_body(source, send, length, sparse)
        .await
        .map_err(|error| error.to_string())
}

async fn reply(send: &mut SendStream, response: Response) -> Result<(), String> {
    wire::write_message(send, &response)
        .await
        .map_err(|error| error.to_string())
}

fn denied(reason: &str) -> Response {
    Response::Denied {
        reason: reason.into(),
    }
}

/// Test-only emulation of a home upload: every byte served, by any stream,
/// takes its turn on one link of `BYTES_PER_SECOND`. Zero means unlimited.
#[cfg(test)]
pub(crate) mod test_link {
    use std::{
        sync::{
            atomic::{AtomicU64, Ordering},
            Mutex,
        },
        time::{Duration, Instant},
    };

    pub static BYTES_PER_SECOND: AtomicU64 = AtomicU64::new(0);
    static NEXT_FREE: Mutex<Option<Instant>> = Mutex::new(None);

    /// Keeps the shared link at its rate. Windows timers tick every ~15.6 ms,
    /// so sleeping for each small piece would undershoot the rate; instead
    /// this sleeps only once the schedule is clearly ahead, which keeps the
    /// average exact with bursts of at most `SLACK`.
    pub async fn pace(bytes: usize) {
        const SLACK: Duration = Duration::from_millis(40);
        if BYTES_PER_SECOND.load(Ordering::Relaxed) > 0 {
            let done = reserve(bytes);
            if done.saturating_duration_since(Instant::now()) > SLACK {
                tokio::time::sleep_until((done - SLACK).into()).await;
            }
        }
    }

    /// When `bytes` will have left the shared link.
    fn reserve(bytes: usize) -> Instant {
        let rate = BYTES_PER_SECOND.load(Ordering::Relaxed).max(1);
        let cost = Duration::from_secs_f64(bytes as f64 / rate as f64);
        let mut next = NEXT_FREE.lock().unwrap_or_else(|e| e.into_inner());
        let now = Instant::now();
        let done = next.map_or(now, |free| free.max(now)) + cost;
        *next = Some(done);
        done
    }
}
