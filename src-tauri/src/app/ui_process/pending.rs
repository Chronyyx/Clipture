use super::wire::{Frame, Message};
use std::{
    collections::HashMap,
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc, Mutex,
    },
    time::Duration,
};
use tokio::sync::oneshot;

type Reply = Result<Frame, String>;
const MAXIMUM_PENDING_REQUESTS: usize = 24;

#[derive(Default)]
struct Requests {
    closed: bool,
    senders: HashMap<u64, oneshot::Sender<Reply>>,
}

#[derive(Default)]
pub struct Pending {
    next: AtomicU64,
    requests: Mutex<Requests>,
}

impl Pending {
    pub fn reserve(self: &Arc<Self>) -> Result<Ticket, String> {
        let mut requests = self
            .requests
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if requests.closed {
            return Err("UI connection is closed".into());
        }
        if requests.senders.len() >= MAXIMUM_PENDING_REQUESTS {
            return Err("UI request limit reached; retry shortly".into());
        }
        let id = self
            .next
            .fetch_add(1, Ordering::Relaxed)
            .checked_add(1)
            .ok_or("UI request IDs exhausted")?;
        let (sender, receiver) = oneshot::channel();
        requests.senders.insert(id, sender);
        Ok(Ticket {
            id,
            receiver: Some(receiver),
            pending: self.clone(),
        })
    }

    pub fn complete(&self, frame: Frame) -> bool {
        let id = match &frame.message {
            Message::Reply { id, .. } | Message::MediaReply { id, .. } => *id,
            _ => return false,
        };
        let sender = self
            .requests
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .senders
            .remove(&id);
        sender.is_some_and(|sender| sender.send(Ok(frame)).is_ok())
    }

    pub fn close(&self, reason: &str) {
        let mut requests = self
            .requests
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        requests.closed = true;
        for (_, sender) in requests.senders.drain() {
            let _ = sender.send(Err(reason.into()));
        }
    }
}

pub struct Ticket {
    pub id: u64,
    receiver: Option<oneshot::Receiver<Reply>>,
    pending: Arc<Pending>,
}

impl Ticket {
    pub async fn wait(mut self, timeout: Duration) -> Reply {
        tokio::time::timeout(
            timeout,
            self.receiver
                .take()
                .expect("ticket may only be awaited once"),
        )
        .await
        .map_err(|_| {
            "UI request timed out; an accepted background operation may still finish".to_string()
        })?
        .map_err(|_| "UI connection closed before replying".to_string())?
    }
}

impl Drop for Ticket {
    fn drop(&mut self) {
        self.pending
            .requests
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .senders
            .remove(&self.id);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cancellation_stale_replies_and_disconnect_release_requests() {
        let pending = Arc::new(Pending::default());
        let cancelled = pending.reserve().unwrap();
        let id = cancelled.id;
        drop(cancelled);
        assert!(!pending.complete(
            Message::Reply {
                id,
                result: Ok(serde_json::Value::Null)
            }
            .into()
        ));
        let outstanding = pending.reserve().unwrap();
        pending.close("fixture EOF");
        let error =
            tauri::async_runtime::block_on(outstanding.wait(Duration::from_secs(1))).unwrap_err();
        assert_eq!(error, "fixture EOF");
        assert!(pending.reserve().is_err());
        assert!(pending.requests.lock().unwrap().senders.is_empty());
    }
    #[test]
    fn pending_work_is_bounded_and_a_dropped_ticket_restores_capacity() {
        let pending = Arc::new(Pending::default());
        let mut tickets: Vec<_> = (0..MAXIMUM_PENDING_REQUESTS)
            .map(|_| pending.reserve().unwrap())
            .collect();
        assert!(pending.reserve().is_err());
        tickets.pop();
        assert!(pending.reserve().is_ok());
        drop(tickets);
        assert!(pending.requests.lock().unwrap().senders.is_empty());
    }
    #[test]
    fn timeout_removes_registration() {
        let pending = Arc::new(Pending::default());
        let ticket = pending.reserve().unwrap();
        assert!(tauri::async_runtime::block_on(ticket.wait(Duration::from_millis(1))).is_err());
        assert!(pending.requests.lock().unwrap().senders.is_empty());
    }
}
