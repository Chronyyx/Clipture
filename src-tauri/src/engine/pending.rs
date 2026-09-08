use std::{collections::HashMap, sync::Mutex, time::Duration};

use serde_json::Value;
use tokio::sync::oneshot;

use crate::error::{AppError, AppResult};

type EngineResponse = Result<Value, String>;
type ResponseSender = oneshot::Sender<EngineResponse>;
pub(super) type ResponseReceiver = oneshot::Receiver<EngineResponse>;

#[derive(Default)]
pub(super) struct PendingResponses {
    senders: Mutex<HashMap<u64, ResponseSender>>,
}

impl PendingResponses {
    pub fn register(&self, id: u64) -> PendingRequest<'_> {
        let (sender, receiver) = oneshot::channel();
        self.senders
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .insert(id, sender);
        PendingRequest {
            owner: self,
            id,
            receiver: Some(receiver),
        }
    }

    pub fn complete(&self, id: u64, response: EngineResponse) -> bool {
        let sender = self
            .senders
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .remove(&id);
        sender.is_some_and(|sender| sender.send(response).is_ok())
    }

    pub fn remove(&self, id: u64) {
        self.senders
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .remove(&id);
    }

    pub fn fail_all(&self, message: &str) {
        let senders = std::mem::take(
            &mut *self
                .senders
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner()),
        );
        for (_, sender) in senders {
            let _ = sender.send(Err(message.to_owned()));
        }
    }

    #[cfg(test)]
    fn len(&self) -> usize {
        self.senders.lock().unwrap().len()
    }
}

/// Registration is a lease: cancellation, failed writes, timeouts, and normal
/// completion all release the sender, including before wait() is first polled.
pub(super) struct PendingRequest<'a> {
    owner: &'a PendingResponses,
    id: u64,
    receiver: Option<ResponseReceiver>,
}

impl PendingRequest<'_> {
    pub async fn wait(mut self, command: &str, timeout: Duration) -> AppResult<Value> {
        match tokio::time::timeout(timeout, self.receiver.take().unwrap()).await {
            Ok(Ok(response)) => response.map_err(AppError::Engine),
            Ok(Err(_)) => Err(AppError::Engine(format!(
                "Engine response channel closed for {command}"
            ))),
            Err(_) => Err(AppError::Engine(format!(
                "Engine request timed out: {command}"
            ))),
        }
    }
}

impl Drop for PendingRequest<'_> {
    fn drop(&mut self) {
        self.owner.remove(self.id);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn child_exit_fails_every_pending_request() {
        tauri::async_runtime::block_on(async {
            let pending = PendingResponses::default();
            let first = pending.register(1);
            let second = pending.register(2);
            pending.fail_all("engine exited");

            assert!(first
                .wait("first", Duration::from_secs(1))
                .await
                .unwrap_err()
                .to_string()
                .contains("engine exited"));
            assert!(second
                .wait("second", Duration::from_secs(1))
                .await
                .unwrap_err()
                .to_string()
                .contains("engine exited"));
            assert_eq!(pending.len(), 0);
        });
    }

    #[test]
    fn timeout_removes_the_abandoned_request() {
        tauri::async_runtime::block_on(async {
            let pending = PendingResponses::default();
            let receiver = pending.register(7);
            let error = receiver
                .wait("fixture", Duration::from_millis(1))
                .await
                .unwrap_err();

            assert!(error.to_string().contains("timed out: fixture"));
            assert_eq!(pending.len(), 0);
        });
    }

    #[test]
    fn dropping_a_request_before_or_during_wait_releases_its_registration() {
        let pending = PendingResponses::default();
        drop(pending.register(1));
        assert_eq!(pending.len(), 0);
        let waiting = pending
            .register(2)
            .wait("cancelled", Duration::from_secs(60));
        assert_eq!(pending.len(), 1);
        drop(waiting);
        assert_eq!(pending.len(), 0);
    }
}
