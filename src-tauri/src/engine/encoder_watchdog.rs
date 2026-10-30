//! Detects an encoder that stopped producing output while work is pending.
//!
//! NVIDIA drivers can freeze NVENC under realtime GPU scheduling with hardware
//! scheduling on and VRAM nearly full (Sunshine documents the same bug). The
//! host watches from outside the engine so a fully hung engine is caught too:
//! a request that gets no answer counts as no progress. Pure state; the client
//! feeds samples and acts on the verdict.
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};

/// Output may pause this long under realtime GPU priority before it is a hang.
pub const BOOSTED_HANG_THRESHOLD: Duration = Duration::from_secs(3);
/// Below realtime the engine shares the NORMAL band, under a focused game's
/// FOCUS band: an uncapped game alone has starved NVENC for 16 s (one encode
/// call took 13.7 s). Only a silence far longer than that counts as a hang.
pub const NORMAL_HANG_THRESHOLD: Duration = Duration::from_secs(60);

/// The engine diagnostics fields the watchdog reads; the rest are ignored.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct EncoderProgress {
    pub engine_running: bool,
    pub encoder_output_packets: u64,
    pub encoder_accepted_frames: u64,
    pub nvenc_in_flight_frames: u64,
    pub queued_frames: u64,
    pub gpu_scheduling_class: String,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Verdict {
    Healthy,
    Stalling,
    Hung(Duration),
}

#[derive(Debug, Default)]
pub struct EncoderWatchdog {
    last: Option<(u64, u64)>,
    scheduling_class: String,
    stalled_since: Option<Instant>,
}

impl EncoderWatchdog {
    /// `sample` is `None` when the engine did not answer in time.
    pub fn observe(&mut self, now: Instant, sample: Option<&EncoderProgress>) -> Verdict {
        let (progressed, work_expected) = match sample {
            None => (false, true),
            Some(sample) => {
                self.scheduling_class.clone_from(&sample.gpu_scheduling_class);
                let current = (sample.encoder_output_packets, sample.encoder_accepted_frames);
                let previous = self.last.replace(current);
                let progressed = previous.is_none_or(|(output, _)| current.0 > output);
                let accepted_more = previous.is_some_and(|(_, accepted)| current.1 > accepted);
                let pending = accepted_more
                    || sample.nvenc_in_flight_frames > 0
                    || sample.queued_frames > 0;
                (progressed, sample.engine_running && pending)
            }
        };
        if progressed || !work_expected {
            self.stalled_since = None;
            return Verdict::Healthy;
        }
        let since = *self.stalled_since.get_or_insert(now);
        let stalled = now.saturating_duration_since(since);
        let threshold = if self.boosted() {
            BOOSTED_HANG_THRESHOLD
        } else {
            NORMAL_HANG_THRESHOLD
        };
        if stalled >= threshold {
            Verdict::Hung(stalled)
        } else {
            Verdict::Stalling
        }
    }

    pub fn boosted(&self) -> bool {
        self.scheduling_class == "realtime"
    }

    /// The class from the engine's configure answer, until a poll reports one.
    /// A freeze before the first poll answer is then judged at the right
    /// threshold instead of the slow one.
    pub fn seed_scheduling_class(&mut self, class: &str) {
        if self.scheduling_class.is_empty() {
            self.scheduling_class = class.to_owned();
        }
    }

    /// The engine's last reported GPU scheduling class ("unknown" before any).
    pub fn scheduling_class(&self) -> &str {
        if self.scheduling_class.is_empty() { "unknown" } else { &self.scheduling_class }
    }

    /// Forget the stall clock (a save or configure is holding the engine).
    pub fn pause(&mut self) {
        self.stalled_since = None;
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum RecoveryState {
    #[default]
    Healthy,
    /// One hang: the engine was restarted at normal GPU priority.
    Recovered,
    /// A second hang: recording stopped until Clipture restarts.
    Halted,
}

/// What happened this session; exported with diagnostics.
#[derive(Clone, Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EncoderRecovery {
    pub state: RecoveryState,
    pub hangs: u32,
    pub last_hang: Option<String>,
}

impl EncoderRecovery {
    /// Records a hang and returns the new state. One automatic recovery per
    /// session; a driver that hangs again will not be fixed by more restarts.
    pub fn record_hang(&mut self, description: String) -> RecoveryState {
        self.hangs += 1;
        self.last_hang = Some(description);
        self.state = if self.state == RecoveryState::Healthy {
            RecoveryState::Recovered
        } else {
            RecoveryState::Halted
        };
        self.state
    }

    /// The boost stays off for the session once a hang was seen.
    pub fn boost_allowed(&self) -> bool {
        self.state == RecoveryState::Healthy
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample(output: u64, accepted: u64, in_flight: u64, class: &str) -> EncoderProgress {
        EncoderProgress {
            engine_running: true,
            encoder_output_packets: output,
            encoder_accepted_frames: accepted,
            nvenc_in_flight_frames: in_flight,
            queued_frames: 0,
            gpu_scheduling_class: class.into(),
        }
    }

    #[test]
    fn steady_output_is_healthy() {
        let start = Instant::now();
        let mut watchdog = EncoderWatchdog::default();
        for second in 0..10 {
            let s = sample(second * 60, second * 60, 2, "realtime");
            let verdict = watchdog.observe(start + Duration::from_secs(second), Some(&s));
            assert_eq!(verdict, Verdict::Healthy);
        }
    }

    #[test]
    fn boosted_hang_is_declared_after_three_seconds() {
        let start = Instant::now();
        let mut watchdog = EncoderWatchdog::default();
        watchdog.observe(start, Some(&sample(100, 100, 16, "realtime")));
        let stuck = sample(100, 100, 16, "realtime");
        assert_eq!(watchdog.observe(start + Duration::from_secs(1), Some(&stuck)), Verdict::Stalling);
        assert_eq!(watchdog.observe(start + Duration::from_secs(3), Some(&stuck)), Verdict::Stalling);
        assert!(matches!(
            watchdog.observe(start + Duration::from_secs(4), Some(&stuck)),
            Verdict::Hung(_)
        ));
    }

    #[test]
    fn below_realtime_tolerates_measured_contention_stalls() {
        // A focused uncapped game starved a `high` engine for 16 s.
        for class in ["normal", "high"] {
            let start = Instant::now();
            let mut watchdog = EncoderWatchdog::default();
            watchdog.observe(start, Some(&sample(100, 100, 16, class)));
            let stuck = sample(100, 100, 16, class);
            // Samples each second, as the client polls; the stall starts at 1 s.
            for second in 1..=60 {
                let verdict = watchdog.observe(start + Duration::from_secs(second), Some(&stuck));
                assert_eq!(verdict, Verdict::Stalling, "{class} at {second}s");
            }
            assert!(matches!(
                watchdog.observe(start + Duration::from_secs(61), Some(&stuck)),
                Verdict::Hung(_)
            ));
            assert_eq!(watchdog.scheduling_class(), class);
        }
    }

    #[test]
    fn idle_encoder_is_not_a_hang() {
        let start = Instant::now();
        let mut watchdog = EncoderWatchdog::default();
        let idle = sample(100, 100, 0, "realtime");
        for second in 0..30 {
            let verdict = watchdog.observe(start + Duration::from_secs(second), Some(&idle));
            assert_eq!(verdict, Verdict::Healthy);
        }
    }

    #[test]
    fn unanswered_requests_count_as_no_progress() {
        let start = Instant::now();
        let mut watchdog = EncoderWatchdog::default();
        watchdog.observe(start, Some(&sample(100, 100, 1, "realtime")));
        assert_eq!(watchdog.observe(start + Duration::from_secs(2), None), Verdict::Stalling);
        assert!(matches!(watchdog.observe(start + Duration::from_secs(5), None), Verdict::Hung(_)));
    }

    #[test]
    fn seeded_realtime_catches_a_freeze_before_any_answer() {
        let start = Instant::now();
        let mut watchdog = EncoderWatchdog::default();
        watchdog.seed_scheduling_class("realtime");
        assert_eq!(watchdog.observe(start, None), Verdict::Stalling);
        assert!(matches!(watchdog.observe(start + Duration::from_secs(3), None), Verdict::Hung(_)));
        assert_eq!(watchdog.scheduling_class(), "realtime");
    }

    #[test]
    fn output_resuming_clears_the_stall() {
        let start = Instant::now();
        let mut watchdog = EncoderWatchdog::default();
        watchdog.observe(start, Some(&sample(100, 100, 16, "realtime")));
        watchdog.observe(start + Duration::from_secs(2), Some(&sample(100, 100, 16, "realtime")));
        let resumed = sample(130, 130, 2, "realtime");
        assert_eq!(watchdog.observe(start + Duration::from_secs(3), Some(&resumed)), Verdict::Healthy);
        let stuck = sample(130, 130, 16, "realtime");
        assert_eq!(watchdog.observe(start + Duration::from_secs(5), Some(&stuck)), Verdict::Stalling);
    }

    #[test]
    fn recovery_happens_once_then_halts() {
        let mut recovery = EncoderRecovery::default();
        assert!(recovery.boost_allowed());
        assert_eq!(recovery.record_hang("first".into()), RecoveryState::Recovered);
        assert!(!recovery.boost_allowed());
        assert_eq!(recovery.record_hang("second".into()), RecoveryState::Halted);
        assert_eq!(recovery.record_hang("third".into()), RecoveryState::Halted);
        assert_eq!(recovery.hangs, 3);
    }
}
