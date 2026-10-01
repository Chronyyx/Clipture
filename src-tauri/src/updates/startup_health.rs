//! Activation health requires both the event loop and configured engine IPC.
//! This does not measure capture performance or require an open WebView.
use std::sync::atomic::{AtomicU8, Ordering};

const HOST: u8 = 1;
const ENGINE: u8 = 2;
const COMPLETE: u8 = HOST | ENGINE;
static READY: Readiness = Readiness(AtomicU8::new(0));

struct Readiness(AtomicU8);
impl Readiness {
    fn mark(&self, stage: u8) -> bool {
        let previous = self.0.fetch_or(stage, Ordering::AcqRel);
        previous != COMPLETE && previous | stage == COMPLETE
    }
}

fn mark(stage: u8) -> Result<(), super::service::UpdateError> {
    if READY.mark(stage) { super::activation::report_healthy()?; }
    Ok(())
}

pub fn host_ready() -> Result<(), super::service::UpdateError> { mark(HOST) }
pub fn engine_ready() -> Result<(), super::service::UpdateError> { mark(ENGINE) }

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn both_stages_required_in_either_order_and_acknowledged_once() {
        for (first, last) in [(HOST, ENGINE), (ENGINE, HOST)] {
            let ready = Readiness(AtomicU8::new(0));
            assert!(!ready.mark(first));
            assert!(!ready.mark(first));
            assert!(ready.mark(last));
            assert!(!ready.mark(last));
            assert!(!ready.mark(first));
        }
    }

    #[test]
    fn racing_stages_acknowledge_exactly_once() {
        let ready = Readiness(AtomicU8::new(0));
        std::thread::scope(|scope| {
            let host = scope.spawn(|| ready.mark(HOST));
            let engine = scope.spawn(|| ready.mark(ENGINE));
            assert_ne!(host.join().unwrap(), engine.join().unwrap());
        });
    }
}
