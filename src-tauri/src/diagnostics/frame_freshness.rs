use super::frame_fields::{number, Counters};
use serde_json::Value;

pub struct Freshness {
    pub bottleneck: &'static str,
    pub rates: Counters,
}

pub fn classify(activity: &Counters, diagnostics: &Value, window_ms: u64) -> Freshness {
    let per_second = |count| {
        if window_ms > 0 {
            count * 1000.0 / window_ms as f64
        } else {
            0.0
        }
    };
    let mut rates: Counters = [
        ("captureClockRequests", "captureClockTickRequests"),
        ("captureClockWakeups", "captureClockTickWakeups"),
        (
            "captureClockCompletionTimeouts",
            "captureClockTickCompletionTimeouts",
        ),
        ("presentLatchWaits", "presentLatchWaits"),
        ("presentLatchHits", "presentLatchHits"),
        ("presentLatchTimeouts", "presentLatchTimeouts"),
        (
            "captureAcquireImmediateMisses",
            "captureAcquireImmediateMisses",
        ),
        ("captureAcquireGraceHits", "captureAcquireGraceHits"),
        ("captureAcquireGraceTimeouts", "captureAcquireGraceTimeouts"),
        ("desktopPresents", "desktopPresents"),
        ("captureUpdates", "captureUpdatesAcquired"),
        ("freshFramesPublished", "freshFramesPublished"),
        ("encoderFramesAccepted", "encoderFramesAccepted"),
        ("encoderPacketsProduced", "encoderPacketsProduced"),
        ("distinctSourceFramesEncoded", "encoderDistinctSourceFrames"),
    ]
    .into_iter()
    .map(|(name, field)| (name, per_second(activity[field])))
    .collect();
    rates.insert("targetFps", number(&diagnostics["fps"]).max(1.0));
    rates.insert(
        "desktopUpdateSupply",
        per_second(activity["desktopPresents"] + activity["accumulatedSourceFrames"]),
    );
    let bottleneck = classify_rates(activity, diagnostics, window_ms, &rates);
    Freshness { bottleneck, rates }
}

fn classify_rates(
    activity: &Counters,
    diagnostics: &Value,
    window_ms: u64,
    rates: &Counters,
) -> &'static str {
    if window_ms == 0 {
        return "collecting";
    }
    let tolerance = 0.92;
    let low_target = rates["targetFps"] * tolerance;
    if matches!(
        diagnostics["captureClockMode"].as_str(),
        Some("encoder-driven-dxgi" | "encoder-prearmed-dxgi")
    ) {
        if rates["captureClockRequests"] < low_target {
            return "capture-clock-request-limited";
        }
        if rates["captureClockWakeups"] < low_target.min(rates["captureClockRequests"] * tolerance)
        {
            return "capture-clock-handoff-limited";
        }
        if rates["captureClockCompletionTimeouts"] >= 1.0_f64.max(rates["targetFps"] * 0.08) {
            return "capture-clock-same-tick-timeout";
        }
        if rates["captureAcquireGraceTimeouts"] >= 1.0_f64.max(rates["targetFps"] * 0.08) {
            return "dxgi-poll-grace-exhausted";
        }
    }
    let expected_published = rates["targetFps"].min(rates["desktopUpdateSupply"]);
    if rates["freshFramesPublished"] < expected_published * tolerance {
        if activity["accumulationEvents"] > 0.0 || activity["accumulatedSourceFrames"] > 0.0 {
            return "capture-acquisition-backlog";
        }
        if rates["captureUpdates"] >= expected_published * tolerance {
            return "capture-sampler-or-publication-limited";
        }
        return "capture-acquisition-limited";
    }
    let expected_input = rates["targetFps"].min(rates["freshFramesPublished"]);
    if rates["encoderFramesAccepted"] < expected_input * tolerance {
        return "encoder-admission-limited";
    }
    let expected_output = expected_input.min(rates["encoderFramesAccepted"]);
    if rates["encoderPacketsProduced"] < expected_output * tolerance {
        return "nvenc-output-limited";
    }
    let expected_distinct = expected_output.min(rates["encoderPacketsProduced"]);
    if rates["distinctSourceFramesEncoded"] < expected_distinct * tolerance {
        if diagnostics["stillFrameDuplicationEnabled"] == true
            && rates["encoderPacketsProduced"] >= low_target
        {
            return "frame-duplication-masking-source-freshness";
        }
        return "encoded-source-freshness-limited";
    }
    if rates["desktopUpdateSupply"] < low_target {
        "healthy-vfr-source-limited"
    } else {
        "healthy"
    }
}

pub fn dominant(reasons: &Counters) -> &'static str {
    let candidates = [
        ("capture-queue-overflow", reasons["captureQueueOverflow"]),
        ("capture-slot-exhaustion", reasons["captureSlotExhaustion"]),
        (
            "scheduler-deadline-miss",
            reasons["schedulerDeadlineMisses"],
        ),
        ("encoder-backpressure", reasons["encoderBackpressure"]),
    ];
    let best = first_max(&candidates);
    if best.1 <= 0.0 {
        return "none";
    }
    if best.0 != "encoder-backpressure" {
        return best.0;
    }
    first_max(&[
        (
            "encoder-backpressure:queue-eviction",
            reasons["encoderQueueDrops"],
        ),
        (
            "encoder-backpressure:nvenc-surface-starvation",
            reasons["nvencSurfaceDrops"],
        ),
        (
            "encoder-backpressure:nvenc-input-busy",
            reasons["nvencInputDrops"],
        ),
        (
            "encoder-backpressure:other",
            reasons["encoderBackpressureOther"],
        ),
    ])
    .0
}
fn first_max(values: &[(&'static str, f64)]) -> (&'static str, f64) {
    values.iter().copied().fold(
        values[0],
        |best, next| if next.1 > best.1 { next } else { best },
    )
}
