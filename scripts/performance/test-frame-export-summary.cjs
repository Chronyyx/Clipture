'use strict';
const assert = require('node:assert/strict');
const { summarize } = require('./summarize-frame-export.cjs');
const sample = (windowMs, output, extras = {}) => ({ windowMs, reasons: { reportedDroppedFrames: 2 },
  freshnessRates: { freshFramesPublished: 180, encoderFramesAccepted: 60,
    encoderPacketsProduced: output, distinctSourceFramesEncoded: output }, ...extras });
const result = summarize({ frameDropAnalysis: { timeline: [sample(1000, 60), sample(3000, 40),
  sample(1000, 0, { reset: true }), sample(1000, 0, { captureEpochChanged: true })] } });
assert.equal(result.averageFps.encoded, 45);
assert.equal(result.averageFps.capturePublished, 180);
assert.equal(result.seconds, 4);
assert.equal(result.skippedResetOrTransitionSamples, 2);
assert.equal(result.reasonCounts.reportedDroppedFrames, 4);
assert.throws(() => summarize({}), /timeline/);
assert.throws(() => summarize({ frameDropAnalysis: { timeline: [sample(1000, 60, { freshnessRates: {} })] } }), /Missing/);
assert.equal(summarize({ frameDropAnalysis: { timeline: [] } }).averageFps, null);
console.log('Frame export summary tests passed.');
