'use strict';
const assert = require('node:assert/strict');
const { summarize } = require('./summarize-cadence-probe.cjs');
const diagnostics = (output, unique, drops = 0) => ({ encoderOutputPackets: output,
  encoderDistinctSourceFrames: unique, capturePublishedFrames: unique,
  encoderQueueDrops: drops, schedulerDroppedFrames: 0, droppedFrames: drops });
const trial = { fps: 120, before: diagnostics(0, 0), after: diagnostics(360, 220, 1),
  diagnosticElapsed: 3, samples: [
    { elapsed: 1, diagnostics: diagnostics(120, 80) },
    { elapsed: 2, diagnostics: diagnostics(220, 140, 1) },
  ] };
const summary = summarize(trial);
assert.equal(summary.windowCount, 3);
assert.equal(summary.minimumWindowOutputFps, 100);
assert.equal(summary.windowsBelow98Percent, 1);
assert.equal(summary.windowsWithDrops, 1);
assert.equal(summary.windows[0].distinctFps, 80);
assert.equal(summarize({ ...trial, samples: [{ elapsed: 1, diagnostics: {} }] }).invalidWindows, 2);
assert.equal(summarize({ ...trial, before: diagnostics(1000, 1000) }).invalidWindows, 1);
assert.equal(summarize({ ...trial, diagnosticElapsed: NaN, samples: [] }).minimumWindowOutputFps, null);
console.log('Cadence probe summary tests passed (short stalls, repeats, missing/reset counters).');
