'use strict';
const assert = require('node:assert/strict');
const { correlate } = require('./correlate-pipeline-window.cjs');
const row = { sampledAt: '1970-01-01T00:00:02Z', windowMs: 1000,
  freshnessRates: { freshFramesPublished: 60, distinctSourceFramesEncoded: 55, encoderPacketsProduced: 120, desktopUpdateSupply: 170 },
  reasons: { encoderQueueDrops: 0 }, context: { captureAcquireP95_100ns: 500,
    nvencCallP95_100ns: 3000, encoderQueueResidenceP95_100ns: 100, schedulerWakeLatenessP95_100ns: 4000 } };
const report = { frameDropAnalysis: { timeline: [row, { ...row, reset: true }, { ...row, captureEpochChanged: true },
  { ...row, sampledAt: '1970-01-01T00:00:01Z' }] } };
const gpu = { samples: [{ receivedAt: '1970-01-01T00:00:01.500Z', 'utilization.gpu': '99',
  'utilization.encoder': '43', 'clocks.current.graphics': '1905' }] };
const result = correlate(report, '', gpu, 1000, 2000);
assert.equal(result.engineWindows, 1);
assert.equal(result.fps.distinctSourceFramesEncoded, 55);
assert.equal(result.maxSampledP95Ms.nvencCallP95_100ns, 0.3);
assert.equal(result.vendor['utilization.gpu'].median, 99);
assert.throws(() => correlate(report, '', gpu, 2000, 1000));
assert.throws(() => correlate(report, '', gpu, 3000, 4000));
assert.throws(() => correlate(report, '', { samples: [] }, 1000, 2000));
console.log('Pipeline correlation interval/reset exclusions and unit conversions passed.');
