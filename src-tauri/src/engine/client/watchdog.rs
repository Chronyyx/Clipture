//! Host-side encoder watchdog: polls the engine from outside, restarts it at
//! normal GPU priority after one hang, and halts recording after a second.
use std::time::Instant;

use super::*;
use crate::engine::encoder_watchdog::{EncoderProgress, EncoderWatchdog, RecoveryState, Verdict};

const WATCHDOG_INTERVAL: Duration = Duration::from_secs(1);
const WATCHDOG_REQUEST_TIMEOUT: Duration = Duration::from_secs(2);
/// A process stuck inside the driver may not exit at once; after this the
/// host stops waiting for it and moves on.
const TERMINATION_GRACE: Duration = Duration::from_secs(5);
/// The watchdog's own probe. Every other request (saves, configure, device
/// lists that a slow driver can hold for seconds) delays the engine's single
/// request loop by design, so while one is in flight silence is not a hang.
const PROBE_REQUEST: &str = "getDiagnostics";

pub(super) const HALTED_MESSAGE: &str =
    "Recording stopped: the GPU encoder stopped responding twice. Restart Clipture to record again.";

/// Marks a long request in flight for the watchdog.
#[derive(Default)]
pub(super) struct LongRequests {
    active: AtomicU64,
    started: AtomicU64,
}

pub(super) struct LongRequestGuard<'a>(&'a LongRequests);

impl LongRequests {
    pub(super) fn enter(&self, command: &str) -> Option<LongRequestGuard<'_>> {
        if command == PROBE_REQUEST {
            return None;
        }
        self.started.fetch_add(1, Ordering::AcqRel);
        self.active.fetch_add(1, Ordering::AcqRel);
        Some(LongRequestGuard(self))
    }

    fn started(&self) -> u64 {
        self.started.load(Ordering::Acquire)
    }

    fn active(&self) -> bool {
        self.active.load(Ordering::Acquire) > 0
    }
}

impl Drop for LongRequestGuard<'_> {
    fn drop(&mut self) {
        self.0.active.fetch_sub(1, Ordering::AcqRel);
    }
}

impl EngineClient {
    pub fn subscribe_encoder_recovery(&self) -> broadcast::Receiver<RecoveryState> {
        self.recovery_events.subscribe()
    }

    pub fn encoder_recovery(&self) -> crate::engine::encoder_watchdog::EncoderRecovery {
        self.recovery.lock().unwrap_or_else(|p| p.into_inner()).clone()
    }

    pub(super) fn recording_halted(&self) -> bool {
        self.recovery.lock().unwrap_or_else(|p| p.into_inner()).state == RecoveryState::Halted
    }

    /// The configure request actually sent: no boost once a hang was seen.
    pub(super) fn effective_configure(&self, mut configure: EngineConfigure) -> EngineConfigure {
        configure.gpu_priority_boost &= self
            .recovery
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .boost_allowed();
        configure
    }

    pub(super) fn spawn_encoder_watchdog(self: &Arc<Self>, generation: u64) {
        let client = Arc::clone(self);
        tauri::async_runtime::spawn(async move { client.watch_encoder(generation).await });
    }

    async fn watch_encoder(self: Arc<Self>, generation: u64) {
        let mut watchdog = EncoderWatchdog::default();
        loop {
            tokio::time::sleep(WATCHDOG_INTERVAL).await;
            if self.shutting_down.load(Ordering::Acquire) || self.running_generation() != Some(generation) {
                return;
            }
            if self.configured_generation.load(Ordering::Acquire) != generation || self.long_requests.active() {
                watchdog.pause();
                continue;
            }
            if let Some(class) = self.cached_diagnostics().details.get("gpuSchedulingClass").and_then(|v| v.as_str()) {
                watchdog.seed_scheduling_class(class);
            }
            let started = self.long_requests.started();
            let sample = self
                .call::<EncoderProgress>("getDiagnostics", &(), WATCHDOG_REQUEST_TIMEOUT)
                .await
                .ok();
            if self.running_generation() != Some(generation) {
                return;
            }
            // A save that started meanwhile delays answers; that is not a hang.
            if self.long_requests.active() || self.long_requests.started() != started {
                watchdog.pause();
                continue;
            }
            if let Verdict::Hung(stalled) = watchdog.observe(Instant::now(), sample.as_ref()) {
                let description = format!(
                    "Encoder made no progress for {:.1}s at {} GPU priority{}.",
                    stalled.as_secs_f64(),
                    watchdog.scheduling_class(),
                    if sample.is_none() { " (engine not answering)" } else { "" }
                );
                self.recover_from_encoder_hang(generation, description).await;
                return;
            }
        }
    }

    async fn recover_from_encoder_hang(self: &Arc<Self>, generation: u64, description: String) {
        let state = self
            .recovery
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .record_hang(description.clone());
        tracing::error!(generation, ?state, "{description}");
        let _ = self.recovery_events.send(state);
        let Some(pid) = self.terminate_generation(generation) else { return };
        tokio::time::sleep(TERMINATION_GRACE).await;
        // Normally the exit event already ran this; it ignores stale generations.
        self.mark_stopped(generation, pid, format!("Native engine stopped after a hang. {description}"));
    }

    /// Ends the live engine of this generation; never a process found by name.
    /// Reads `live`, not `process`: a request write stuck on this very engine
    /// can hold `process` indefinitely.
    fn terminate_generation(&self, generation: u64) -> Option<u32> {
        let (live_generation, pid) = (*self.live.lock().unwrap_or_else(|p| p.into_inner()))?;
        if live_generation != generation {
            return None;
        }
        if let Err(error) = crate::platform::windows::terminate_process(pid, 92) {
            tracing::error!(%error, pid, "could not stop the hung native engine");
        }
        Some(pid)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_request_but_the_probe_marks_the_engine_busy() {
        let requests = LongRequests::default();
        assert!(requests.enter(PROBE_REQUEST).is_none());
        for command in ["saveClip", "configure", "listAudioInputDevices", "listRunningProcesses", "configureHotkey"] {
            let started = requests.started();
            let guard = requests.enter(command);
            assert!(guard.is_some() && requests.active(), "{command}");
            assert_eq!(requests.started(), started + 1);
            drop(guard);
            assert!(!requests.active(), "{command} released");
        }
    }
}
