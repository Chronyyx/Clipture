const assert = require("node:assert/strict");
const { FrameDropDiagnosticsRecorder, classifyVisualFreshness } = require("../dist/main/FrameDropDiagnostics.js");

function activity(overrides = {}) {
  return {
    captureUpdatesAcquired: 60,
    desktopPresents: 60,
    pointerUpdates: 0,
    freshFramesPublished: 60,
    accumulatedSourceFrames: 0,
    accumulationEvents: 0,
    captureAcquireImmediateMisses: 0,
    captureAcquireGraceHits: 0,
    captureAcquireGraceTimeouts: 0,
    captureClockTickRequests: 60,
    captureClockTickWakeups: 60,
    captureClockTickCoalesced: 0,
    captureClockTickCompletions: 60,
    captureClockTickCompletionWaits: 60,
    captureClockTickCompletionTimeouts: 0,
    presentLatchWaits: 0,
    presentLatchHits: 0,
    presentLatchTimeouts: 0,
    catchUpEvents: 0,
    historicalFramesRecovered: 0,
    catchUpRepeatedTicks: 0,
    encoderAdmissionRejections: 0,
    encoderFramesAccepted: 60,
    encoderPacketsProduced: 60,
    encoderDistinctSourceFrames: 60,
    encoderRepeatedSourceFrames: 0,
    encoderUnknownSourceFrames: 0,
    replayPacketsPersisted: 60,
    ...overrides
  };
}

function classify(overrides = {}, diagnostics = {}) {
  return classifyVisualFreshness(
    activity(overrides),
    { fps: 60, stillFrameDuplicationEnabled: false, captureClockMode: "capture-sampled", ...diagnostics },
    1000
  ).bottleneck;
}

assert.equal(classify(), "healthy");
assert.equal(
  classify(
    { captureClockTickRequests: 40, captureClockTickWakeups: 40 },
    { captureClockMode: "encoder-driven-dxgi" }
  ),
  "capture-clock-request-limited"
);
assert.equal(
  classify(
    { captureClockTickWakeups: 40, captureClockTickCoalesced: 20 },
    { captureClockMode: "encoder-driven-dxgi" }
  ),
  "capture-clock-handoff-limited"
);
assert.equal(
  classify(
    { captureClockTickCompletionTimeouts: 8 },
    { captureClockMode: "encoder-prearmed-dxgi" }
  ),
  "capture-clock-same-tick-timeout"
);
assert.equal(
  classify(
    { captureAcquireImmediateMisses: 8, captureAcquireGraceTimeouts: 8 },
    { captureClockMode: "encoder-driven-dxgi" }
  ),
  "dxgi-poll-grace-exhausted"
);
assert.equal(
  classify({
    desktopPresents: 45,
    captureUpdatesAcquired: 45,
    freshFramesPublished: 45,
    encoderFramesAccepted: 45,
    encoderPacketsProduced: 45,
    encoderDistinctSourceFrames: 45
  }),
  "healthy-vfr-source-limited"
);
assert.equal(
  classify({
    desktopPresents: 0,
    captureUpdatesAcquired: 0,
    freshFramesPublished: 0,
    encoderFramesAccepted: 0,
    encoderPacketsProduced: 0,
    encoderDistinctSourceFrames: 0
  }),
  "healthy-vfr-source-limited"
);
assert.equal(
  classify({
    desktopPresents: 45,
    captureUpdatesAcquired: 45,
    freshFramesPublished: 45,
    encoderFramesAccepted: 45,
    encoderPacketsProduced: 30,
    encoderDistinctSourceFrames: 30
  }),
  "nvenc-output-limited"
);
assert.equal(
  classify({
    desktopPresents: 30,
    captureUpdatesAcquired: 30,
    freshFramesPublished: 30,
    accumulatedSourceFrames: 40,
    accumulationEvents: 1,
    encoderFramesAccepted: 30,
    encoderPacketsProduced: 30,
    encoderDistinctSourceFrames: 30
  }),
  "capture-acquisition-backlog"
);
assert.equal(
  classify({ freshFramesPublished: 40, accumulatedSourceFrames: 4, accumulationEvents: 1 }),
  "capture-acquisition-backlog"
);
assert.equal(
  classify({ freshFramesPublished: 40 }),
  "capture-sampler-or-publication-limited"
);
assert.equal(
  classify({ captureUpdatesAcquired: 30, freshFramesPublished: 30 }),
  "capture-acquisition-limited"
);
assert.equal(
  classify({ encoderFramesAccepted: 40, encoderPacketsProduced: 40, encoderDistinctSourceFrames: 40 }),
  "encoder-admission-limited"
);
assert.equal(
  classify({ encoderPacketsProduced: 40, encoderDistinctSourceFrames: 40 }),
  "nvenc-output-limited"
);
assert.equal(
  classify({ encoderDistinctSourceFrames: 30, encoderRepeatedSourceFrames: 30 }, { stillFrameDuplicationEnabled: true }),
  "frame-duplication-masking-source-freshness"
);
assert.equal(
  classify({ encoderDistinctSourceFrames: 30 }),
  "encoded-source-freshness-limited"
);

const recorder = new FrameDropDiagnosticsRecorder(0);
const telemetryBase = {
  fps: 60,
  captureEpoch: 1,
  captureClockMode: "capture-sampled",
  stillFrameDuplicationEnabled: false,
  catchUpEvents: 4,
  historicalFramesRecovered: 3,
  catchUpRepeatedTicks: 1,
  encoderAdmissionRejections: 2
};
recorder.observe(telemetryBase, 1_000);
const recentTelemetry = recorder.observe({
  ...telemetryBase,
  catchUpEvents: 6,
  historicalFramesRecovered: 5,
  catchUpRepeatedTicks: 2,
  encoderAdmissionRejections: 5
}, 2_000);
assert.equal(recentTelemetry.recentCatchUpEvents, 2);
assert.equal(recentTelemetry.recentHistoricalFramesRecovered, 2);
assert.equal(recentTelemetry.recentCatchUpRepeatedTicks, 1);
assert.equal(recentTelemetry.recentEncoderAdmissionRejections, 3);

console.log("Frame freshness diagnostics tests passed.");
