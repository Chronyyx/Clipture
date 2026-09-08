use super::{frame_fields::*, frame_freshness};
use crate::{contracts::EngineDiagnostics, library::iso_utc};
use serde::Serialize;
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, VecDeque},
    time::{Duration, UNIX_EPOCH},
};

struct Baseline {
    sampled_at_ms: u64,
    epoch: f64,
    reasons: Counters,
    activity: Counters,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct Sample {
    sampled_at: String,
    window_ms: u64,
    reset: bool,
    capture_epoch_changed: bool,
    dominant_counted_reason: &'static str,
    visual_freshness_bottleneck: &'static str,
    counted_reason_total: f64,
    accounting_difference: f64,
    reported_drops_per_second: f64,
    freshness_rates: Counters,
    reasons: Counters,
    activity: Counters,
    context: BTreeMap<&'static str, Value>,
}

/// Bounded host-owned history; sampling continues without any renderer.
pub struct FrameDropRecorder {
    minimum_interval_ms: u64,
    maximum_samples: usize,
    baseline: Option<Baseline>,
    timeline: VecDeque<Sample>,
}

impl Default for FrameDropRecorder {
    fn default() -> Self {
        Self::new(1000, 900)
    }
}

impl FrameDropRecorder {
    pub fn new(minimum_interval_ms: u64, maximum_samples: usize) -> Self {
        Self {
            minimum_interval_ms,
            maximum_samples: maximum_samples.max(1),
            baseline: None,
            timeline: VecDeque::new(),
        }
    }

    pub fn observe(&mut self, diagnostics: &mut EngineDiagnostics, sampled_at_ms: u64) {
        let values = serde_json::to_value(&*diagnostics).expect("diagnostics are JSON values");
        let current = Baseline {
            sampled_at_ms,
            epoch: number(&values["captureEpoch"]).trunc().max(0.0),
            reasons: counters(&values, REASONS),
            activity: counters(&values, ACTIVITY),
        };
        if let Some(previous) = &self.baseline {
            let window_ms = sampled_at_ms.saturating_sub(previous.sampled_at_ms);
            if window_ms < self.minimum_interval_ms {
                self.enrich(diagnostics);
                return;
            }
            let reset = decreased(&current.reasons, &previous.reasons)
                || decreased(&current.activity, &previous.activity);
            let mut reasons = delta(&current.reasons, &previous.reasons, reset);
            reasons.insert(
                "encoderBackpressureOther",
                (reasons["encoderBackpressure"]
                    - reasons["encoderQueueDrops"]
                    - reasons["nvencSurfaceDrops"]
                    - reasons["nvencInputDrops"])
                    .max(0.0),
            );
            let activity = delta(&current.activity, &previous.activity, reset);
            let counted = reasons["captureQueueOverflow"]
                + reasons["captureSlotExhaustion"]
                + reasons["schedulerDeadlineMisses"]
                + reasons["encoderBackpressure"];
            let freshness = frame_freshness::classify(&activity, &values, window_ms);
            let epoch_changed = current.epoch != previous.epoch;
            let sample = Sample {
                sampled_at: iso_utc(UNIX_EPOCH + Duration::from_millis(sampled_at_ms)),
                window_ms,
                reset,
                capture_epoch_changed: epoch_changed,
                dominant_counted_reason: frame_freshness::dominant(&reasons),
                visual_freshness_bottleneck: if reset {
                    "counter-reset"
                } else if epoch_changed {
                    "capture-epoch-transition"
                } else {
                    freshness.bottleneck
                },
                counted_reason_total: counted,
                accounting_difference: reasons["reportedDroppedFrames"] - counted,
                reported_drops_per_second: if window_ms > 0 {
                    reasons["reportedDroppedFrames"] * 1000.0 / window_ms as f64
                } else {
                    0.0
                },
                freshness_rates: freshness.rates,
                reasons,
                activity,
                context: CONTEXT
                    .iter()
                    .filter_map(|(name, field)| {
                        values.get(*field).map(|value| (*name, value.clone()))
                    })
                    .collect(),
            };
            self.timeline.push_back(sample);
            while self.timeline.len() > self.maximum_samples {
                self.timeline.pop_front();
            }
        }
        self.baseline = Some(current);
        self.enrich(diagnostics);
    }

    fn enrich(&self, diagnostics: &mut EngineDiagnostics) {
        let latest = self.timeline.back();
        diagnostics.details.insert(
            "recentDropWindowMs".into(),
            json!(latest.map_or(0, |sample| sample.window_ms)),
        );
        for (target, reason) in RECENT_REASONS {
            diagnostics.details.insert(
                (*target).into(),
                json!(latest.map_or(0.0, |sample| sample.reasons[reason])),
            );
        }
        for (target, activity) in RECENT_ACTIVITY {
            diagnostics.details.insert(
                (*target).into(),
                json!(latest.map_or(0.0, |sample| sample.activity[activity])),
            );
        }
        diagnostics.details.insert(
            "recentDropDominantReason".into(),
            json!(latest.map_or("none", |sample| sample.dominant_counted_reason)),
        );
        diagnostics.details.insert(
            "recentVisualFreshnessBottleneck".into(),
            json!(latest.map_or("collecting", |sample| sample.visual_freshness_bottleneck)),
        );
    }

    pub fn report(&self) -> Value {
        let mut totals = counters(&Value::Null, REASONS);
        for sample in &self.timeline {
            for (reason, value) in &sample.reasons {
                *totals.get_mut(reason).unwrap() += value;
            }
        }
        let definitions: Value =
            serde_json::from_str(include_str!("frame_definitions.json")).unwrap();
        json!({
            "sampleIntervalMinimumMs": self.minimum_interval_ms,
            "maximumRetainedSamples": self.maximum_samples,
            "retainedWindowMs": self.timeline.iter().map(|sample| sample.window_ms).sum::<u64>(),
            "definitions": definitions, "totals": totals, "latest": self.timeline.back(), "timeline": self.timeline,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // JS and Rust serialize whole numbers differently; compare numeric values,
    // while requiring identical keys, arrays, strings, and null semantics.
    fn equivalent(actual: &Value, expected: &Value) {
        match (actual, expected) {
            (Value::Number(a), Value::Number(b)) => {
                assert!((a.as_f64().unwrap() - b.as_f64().unwrap()).abs() < 1e-8)
            }
            (Value::Array(a), Value::Array(b)) => {
                assert_eq!(a.len(), b.len());
                for (a, b) in a.iter().zip(b) {
                    equivalent(a, b);
                }
            }
            (Value::Object(a), Value::Object(b)) => {
                assert_eq!(
                    a.len(),
                    b.len(),
                    "object keys: actual={:?}, expected={:?}",
                    a.keys(),
                    b.keys()
                );
                for (key, value) in b {
                    equivalent(a.get(key).unwrap_or_else(|| panic!("missing {key}")), value);
                }
            }
            _ => assert_eq!(actual, expected),
        }
    }

    #[test]
    fn freshness_matches_electron_golden_cases() {
        let fixture: Value = serde_json::from_str(include_str!(
            "../../../scripts/migration/fixtures/frame-freshness.v1.json"
        ))
        .unwrap();
        for case in fixture.as_array().unwrap() {
            let activity: Counters = ACTIVITY
                .iter()
                .map(|(name, _)| (*name, number(&case["activity"][*name])))
                .collect();
            let actual = frame_freshness::classify(
                &activity,
                &case["diagnostics"],
                case["windowMs"].as_u64().unwrap(),
            );
            equivalent(
                &json!({"bottleneck":actual.bottleneck,"rates":actual.rates}),
                &case["expected"],
            );
        }
    }

    #[test]
    fn bounded_history_resets_epochs_and_enrichment_match_electron() {
        let fixture: Value = serde_json::from_str(include_str!(
            "../../../scripts/migration/fixtures/frame-recorder.v1.json"
        ))
        .unwrap();
        let mut recorder = FrameDropRecorder::new(
            fixture["minimumIntervalMs"].as_u64().unwrap(),
            fixture["maximumSamples"].as_u64().unwrap() as usize,
        );
        for sample in fixture["observations"].as_array().unwrap() {
            let mut diagnostics: EngineDiagnostics =
                serde_json::from_value(sample["diagnostics"].clone()).unwrap();
            recorder.observe(&mut diagnostics, sample["at"].as_u64().unwrap());
            equivalent(
                &serde_json::to_value(diagnostics).unwrap(),
                &sample["expected"],
            );
        }
        equivalent(&recorder.report(), &fixture["report"]);
    }
}
