'use strict';
const assert = require('node:assert/strict');
const { summarize } = require('./summarize-pipeline-trace.cjs');
const row = { stage: 'idle-total', kind: 'cpu', utcMs: 1000, windowMs: 1000,
  count: 2, avgMs: 1, maxMs: 2, over1Ms: 1, over5Ms: 0, over10Ms: 0 };
const line = r => `[pipeline-trace] ${JSON.stringify(r)}\n`;
const raw = line(row) + line({ ...row, utcMs: 2000, count: 1, avgMs: 7, maxMs: 7, over5Ms: 1 });
const result = summarize(raw);
assert.equal(result.stages[0].weightedAvgMs, 3);
assert.equal(result.stages[0].maxMs, 7);
assert.equal(result.stages[0].count, 3);
assert.equal(result.rejected, 0);
assert.equal(summarize(raw, 1500, 2500).excluded, 1);
assert.throws(() => summarize(raw, NaN));
assert.throws(() => summarize(raw, 2000, 1000));
const prefix = '2026-09-13T08:01:00.123456Z  WARN clipture_engine: ';
const chunks = raw.match(/.{1,13}/g).map(c => prefix + c + '\r\n').join('');
assert.deepEqual(summarize(chunks), result);
assert.equal(summarize('[pipeline-trace] bad\n' + line(row)).rejected, 1);
assert.equal(summarize(line({ ...row, count: -1 })).rejected, 1);
assert.equal(summarize(line({ ...row, count: 0 })).rejected, 1);
const health = summarize(line({ stage: 'capture-copy', kind: 'gpu-health', utcMs: 1000,
  completed: 10, skipped: 2, pending: 8, oldestPendingMs: 803 }));
assert.equal(health.stages[0].maxPending, 8);
assert.equal(health.stages[0].lastSkipped, 2);
assert.equal(summarize('').records.length, 0);
console.log('Pipeline trace parsing, chunk reconstruction, UTC filtering and weighted summaries passed.');
