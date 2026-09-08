//! Explicit telemetry field mappings pinned to the Electron behavior reference.
use serde_json::Value;
use std::collections::BTreeMap;
pub type Counters = BTreeMap<&'static str, f64>;

pub const REASONS: &[(&str, &str)] = &[
    ("reportedDroppedFrames", "droppedFrames"),
    ("captureQueueOverflow", "captureOverflowDrops"),
    ("captureSlotExhaustion", "captureSlotDrops"),
    ("schedulerDeadlineMisses", "schedulerDroppedFrames"),
    ("captureCallbackErrors", "captureCallbackErrors"),
    ("captureQueueCoalesced", "captureCoalescedDrops"),
    ("sourceFramesSuperseded", "sourceFramesSuperseded"),
    ("captureSamplerRejections", "captureSamplerRejections"),
    (
        "captureNonMonotonicTimestamps",
        "captureNonMonotonicTimestamps",
    ),
    ("captureAcquireTimeouts", "captureAcquireTimeouts"),
    ("captureAccessLosses", "captureAccessLosses"),
    ("captureFallbacks", "captureFallbacks"),
    ("schedulerRepeats", "schedulerRepeatedFrames"),
    ("encoderRepeatsCoalesced", "encoderRepeatCoalesced"),
    ("encoderBackpressure", "encoderBackpressureDrops"),
    ("encoderQueueDrops", "encoderQueueDrops"),
    ("nvencSurfaceDrops", "nvencSurfaceDrops"),
    ("nvencInputDrops", "nvencInputDrops"),
    ("encoderBackpressureOther", ""),
];

pub const ACTIVITY: &[(&str, &str)] = &[
    ("captureUpdatesAcquired", "captureAcquiredUpdates"),
    ("desktopPresents", "captureDesktopPresents"),
    ("pointerUpdates", "capturePointerUpdates"),
    ("freshFramesPublished", "capturePublishedFrames"),
    ("accumulatedSourceFrames", "captureAccumulatedFrames"),
    ("accumulationEvents", "captureAccumulationEvents"),
    (
        "captureAcquireImmediateMisses",
        "captureAcquireImmediateMisses",
    ),
    ("captureAcquireGraceHits", "captureAcquireGraceHits"),
    ("captureAcquireGraceTimeouts", "captureAcquireGraceTimeouts"),
    ("captureClockTickRequests", "captureClockTickRequests"),
    ("captureClockTickWakeups", "captureClockTickWakeups"),
    ("captureClockTickCoalesced", "captureClockTickCoalesced"),
    ("captureClockTickCompletions", "captureClockTickCompletions"),
    (
        "captureClockTickCompletionWaits",
        "captureClockTickCompletionWaits",
    ),
    (
        "captureClockTickCompletionTimeouts",
        "captureClockTickCompletionTimeouts",
    ),
    ("presentLatchWaits", "presentLatchWaits"),
    ("presentLatchHits", "presentLatchHits"),
    ("presentLatchTimeouts", "presentLatchTimeouts"),
    ("catchUpEvents", "catchUpEvents"),
    ("historicalFramesRecovered", "historicalFramesRecovered"),
    ("catchUpRepeatedTicks", "catchUpRepeatedTicks"),
    ("encoderAdmissionRejections", "encoderAdmissionRejections"),
    ("encoderFramesAccepted", "encoderAcceptedFrames"),
    ("encoderPacketsProduced", "encoderOutputPackets"),
    ("encoderDistinctSourceFrames", "encoderDistinctSourceFrames"),
    ("encoderRepeatedSourceFrames", "encoderRepeatedSourceFrames"),
    ("encoderUnknownSourceFrames", "encoderUnknownSourceFrames"),
    ("replayPacketsPersisted", "replayArchivePersistedPackets"),
];

pub const CONTEXT: &[(&str, &str)] = &[
    ("captureEpoch", "captureEpoch"),
    ("capturePressure", "capturePressure"),
    ("activeCaptureBackend", "activeCaptureBackend"),
    ("captureClockMode", "captureClockMode"),
    ("desktopPresentFps", "desktopPresentFps"),
    ("publishedFreshFps", "publishedFreshFps"),
    ("recentCaptureFps", "recentPublishedFreshFps"),
    ("recentEncoderInputFps", "recentEncoderInputFps"),
    ("recentEncoderOutputFps", "recentEncoderOutputFps"),
    (
        "recentEncoderDistinctSourceFps",
        "recentEncoderDistinctSourceFps",
    ),
    (
        "stillFrameDuplicationEnabled",
        "stillFrameDuplicationEnabled",
    ),
    (
        "captureSourceIntervalP95_100ns",
        "recentCaptureSourceIntervalP95_100ns",
    ),
    (
        "publishedWallIntervalP95_100ns",
        "recentPublishedWallIntervalP95_100ns",
    ),
    (
        "schedulerWakeLatenessP95_100ns",
        "recentSchedulerWakeLatenessP95_100ns",
    ),
    (
        "encoderQueueResidenceP95_100ns",
        "recentEncoderQueueResidenceP95_100ns",
    ),
    (
        "encoderInputIntervalP95_100ns",
        "recentEncoderInputIntervalP95_100ns",
    ),
    (
        "encoderOutputIntervalP95_100ns",
        "recentEncoderOutputIntervalP95_100ns",
    ),
    ("queuedFrames", "queuedFrames"),
    ("encoderQueuedFreshFrames", "encoderQueuedFreshFrames"),
    ("encoderQueuedRepeatFrames", "encoderQueuedRepeatFrames"),
    ("nvencInFlightFrames", "nvencInFlightFrames"),
    ("captureAcquireP95_100ns", "recentCaptureAcquireP95_100ns"),
    (
        "capturePreparationP95_100ns",
        "recentCapturePreparationP95_100ns",
    ),
    (
        "captureProcessingP95_100ns",
        "recentCaptureProcessingP95_100ns",
    ),
    (
        "inputPreparationP95_100ns",
        "recentInputPreparationP95_100ns",
    ),
    ("inputMapP95_100ns", "recentInputMapP95_100ns"),
    ("nvencCallP95_100ns", "recentNvencCallP95_100ns"),
    ("outputEventWaitP95_100ns", "recentOutputEventWaitP95_100ns"),
    ("outputLockP95_100ns", "recentOutputLockP95_100ns"),
    ("outputCopyP95_100ns", "recentOutputCopyP95_100ns"),
    ("outputUnmapP95_100ns", "recentOutputUnmapP95_100ns"),
    ("maximumCaptureGap100ns", "maximumCaptureGap100ns"),
    ("maximumSubmitLatency100ns", "maximumSubmitLatency100ns"),
    ("replayArchiveQueuedBytes", "replayArchiveQueuedBytes"),
    ("replayArchiveQueuedPackets", "replayArchiveQueuedPackets"),
    ("replayArchiveWriteFailures", "replayArchiveWriteFailures"),
];

pub const RECENT_REASONS: &[(&str, &str)] = &[
    ("recentDroppedFrames", "reportedDroppedFrames"),
    ("recentCaptureOverflowDrops", "captureQueueOverflow"),
    ("recentCaptureSlotDrops", "captureSlotExhaustion"),
    ("recentSchedulerDroppedFrames", "schedulerDeadlineMisses"),
    ("recentEncoderBackpressureDrops", "encoderBackpressure"),
    (
        "recentEncoderBackpressureOtherDrops",
        "encoderBackpressureOther",
    ),
    ("recentCaptureCallbackErrors", "captureCallbackErrors"),
    ("recentCaptureCoalescedDrops", "captureQueueCoalesced"),
    ("recentSourceFramesSuperseded", "sourceFramesSuperseded"),
    ("recentCaptureSamplerRejections", "captureSamplerRejections"),
    (
        "recentCaptureNonMonotonicTimestamps",
        "captureNonMonotonicTimestamps",
    ),
    ("recentCaptureAcquireTimeouts", "captureAcquireTimeouts"),
    ("recentCaptureAccessLosses", "captureAccessLosses"),
    ("recentCaptureFallbacks", "captureFallbacks"),
    ("recentSchedulerRepeatedFrames", "schedulerRepeats"),
    ("recentEncoderQueueDrops", "encoderQueueDrops"),
    ("recentEncoderRepeatCoalesced", "encoderRepeatsCoalesced"),
    ("recentNvencSurfaceDrops", "nvencSurfaceDrops"),
    ("recentNvencInputDrops", "nvencInputDrops"),
];

pub const RECENT_ACTIVITY: &[(&str, &str)] = &[
    ("recentCatchUpEvents", "catchUpEvents"),
    (
        "recentHistoricalFramesRecovered",
        "historicalFramesRecovered",
    ),
    ("recentCatchUpRepeatedTicks", "catchUpRepeatedTicks"),
    (
        "recentEncoderAdmissionRejections",
        "encoderAdmissionRejections",
    ),
];

pub fn counters(values: &Value, fields: &'static [(&str, &str)]) -> Counters {
    fields
        .iter()
        .map(|(name, source)| (*name, number(&values[*source]).trunc().max(0.0)))
        .collect()
}
pub fn number(value: &Value) -> f64 {
    value.as_f64().filter(|n| n.is_finite()).unwrap_or(0.0)
}
pub fn delta(current: &Counters, previous: &Counters, reset: bool) -> Counters {
    current
        .iter()
        .map(|(name, value)| {
            (
                *name,
                if reset {
                    0.0
                } else {
                    (value - previous[name]).max(0.0)
                },
            )
        })
        .collect()
}
pub fn decreased(current: &Counters, previous: &Counters) -> bool {
    current.iter().any(|(name, value)| *value < previous[name])
}
