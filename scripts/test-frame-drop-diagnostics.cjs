const assert = require("node:assert/strict");
const { classifyVisualFreshness } = require("../dist/main/FrameDropDiagnostics.js");

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
  classify({ desktopPresents: 30, captureUpdatesAcquired: 30 }),
  "desktop-source-present-limited"
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
  classify({ encoderFramesAccepted: 40, encoderPacketsProduced: 40, encoderDistinctSourceFrames: 40 }),
  "scheduler-admission-limited"
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

console.log("Frame freshness diagnostics tests passed.");
