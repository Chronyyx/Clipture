use crate::contracts::HotkeyEvent;
use tokio::sync::broadcast::{error::RecvError, Receiver};

/// A slow save may overflow the bounded event channel. Lag is recoverable;
/// only channel closure should terminate the background hotkey listener.
pub(super) async fn next_trigger(receiver: &mut Receiver<HotkeyEvent>) -> bool {
    loop {
        match receiver.recv().await {
            Ok(_) => return true,
            Err(RecvError::Lagged(skipped)) => {
                tracing::warn!(
                    skipped,
                    "hotkey listener skipped queued events during a slow save"
                );
            }
            Err(RecvError::Closed) => return false,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn overflow_does_not_permanently_disable_hotkey_saves() {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .build()
            .unwrap();
        runtime.block_on(async {
            let (sender, mut receiver) = tokio::sync::broadcast::channel(1);
            for _ in 0..10 {
                sender
                    .send(HotkeyEvent {
                        source: "fixture".into(),
                    })
                    .unwrap();
            }
            assert!(next_trigger(&mut receiver).await);
            sender
                .send(HotkeyEvent {
                    source: "after-overflow".into(),
                })
                .unwrap();
            assert!(next_trigger(&mut receiver).await);
            drop(sender);
            assert!(!next_trigger(&mut receiver).await);
        });
    }
}
