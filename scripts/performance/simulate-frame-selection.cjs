'use strict';
// Offline metadata replay only. No video, GPU, settings, or recorder access.
const fs = require('node:fs');
const assert = require('node:assert/strict');
const fields = ['kind', 'at', 'source', 'deadline', 'epoch', 'sequence', 'depth', 'coalesced', 'overflow', 'fps'];
function parse(text) {
  const lines = text.trim().split(/\r?\n/);
  const meta = JSON.parse(lines.shift().replace(/^# /, ''));
  assert.equal(meta.version, 1); assert.equal(meta.complete, true, 'Partial trace');
  assert.equal(meta.lost, 0, 'Lost/overflowed trace events');
  assert.equal(lines.shift(), fields.join(','));
  const events = lines.map(line => Object.fromEntries(line.split(',').map((v, i) => [fields[i], i ? Number(v) : v])));
  let previous = -Infinity;
  for (const e of events) {
    assert.equal(Object.keys(e).length, fields.length);
    assert.ok(fields.slice(1).every(k => Number.isSafeInteger(e[k])), 'Invalid/unsafe integer');
    assert.ok(['p', 't'].includes(e.kind), 'Transition/unsupported consumer: record a steady-state trial');
    assert.ok(e.at >= previous, 'Nonmonotonic arrival order'); previous = e.at;
  }
  return { meta, events };
}
const id = e => e ? `${e.epoch}:${e.sequence}` : '0:0';
function verify({ meta, events }) {
  const queue = []; let lastSource = -Infinity;
  const epochs = new Set(), rates = new Set();
  for (const e of events) {
    assert.equal(e.depth, queue.length, 'Queue depth mismatch');
    if (e.kind === 'p') {
      epochs.add(e.epoch); assert.ok(e.sequence > 0 && e.source > lastSource, 'Invalid source order');
      lastSource = e.source;
      const overflow = Math.max(0, queue.length - meta.queueCapacity + 1);
      assert.equal(e.overflow, overflow); queue.splice(0, overflow); queue.push(e);
    } else {
      rates.add(e.fps);
      assert.equal(id(queue.at(-1)), id(e), 'Actual latest-frame selection does not replay');
      assert.equal(e.coalesced, Math.max(0, queue.length - 1)); queue.length = 0;
    }
  }
  assert.equal(epochs.size, 1, 'Epoch transition'); assert.equal(rates.size, 1, 'FPS transition');
  const fps = [...rates][0]; assert.ok(fps >= 24 && fps <= 240);
  return fps;
}
const percentile = (values, p) => values.length ? [...values].sort((a, b) => a - b)[Math.min(values.length - 1, Math.floor(values.length * p))] : null;
function replay(trace, { mode = 'latest', delayMs = 0 }) {
  const fps = verify(trace), delay = Math.round(delayMs * 10000);
  const frames = trace.events.filter(e => e.kind === 'p');
  const ticks = trace.events.filter(e => e.kind === 't');
  let cursor = 0, held = null, previous = '', repeats = 0, count = 0, overflow = 0, maxDepth = 0;
  const queue = [], ages = [], errors = [];
  // Common scoring interval avoids warmup/end lookahead bias across variants.
  const start = ticks[0].at + 1e7, end = ticks.at(-1).at - 1e7;
  assert.ok(end > start, 'Need more than two seconds of ticks');
  for (const tick of ticks) {
    const at = tick.at + (mode === 'latest' ? delay : 0);
    while (cursor < frames.length && frames[cursor].at <= at) {
      if (queue.length >= trace.meta.queueCapacity) { queue.shift(); if (tick.at >= start && tick.at <= end) ++overflow; }
      queue.push(frames[cursor++]); maxDepth = Math.max(maxDepth, queue.length);
    }
    if (mode === 'latest') { if (queue.length) held = queue.at(-1); queue.length = 0; }
    else {
      // Timestamp-based delayed resampling, not a FIFO that can grow arbitrarily stale.
      const target = tick.deadline - delay;
      while (queue.length && queue[0].source <= target) held = queue.shift();
    }
    if (!held || tick.at < start || tick.at > end) continue;
    ++count; if (id(held) === previous) ++repeats; previous = id(held);
    ages.push((at - held.source) / 10000);
    errors.push((tick.deadline - (mode === 'source-time' ? delay : 0) - held.source) / 10000);
  }
  return { mode, delayMs, scoredSeconds: (end - start) / 1e7, fps,
    outputTicks: count, distinctSelections: count - repeats, repeats,
    distinctSelectionsPerSecond: (count - repeats) / ((end - start) / 1e7),
    sourceAgeMs: { p50: percentile(ages, .5), p95: percentile(ages, .95), max: Math.max(...ages) },
    targetErrorMs: { p05: percentile(errors, .05), p95: percentile(errors, .95) },
    queueOverflow: overflow, maximumRetainedQueueDepth: maxDepth };
}
function analyze(trace) {
  const fps = verify(trace);
  return { meta: trace.meta, actualSelectionReplay: 'passed',
    caveat: 'Counterfactual selection only. GPU scheduling/lease pressure, encoder throughput, A/V synchronization and game impact are not simulated. No fabricated source IDs. Higher distinct count alone does not establish smoother motion.',
    variants: [...[0, .5, 1, 2, 4].map(delayMs => replay(trace, { delayMs })),
      ...[1000 / fps, 2000 / fps].map(delayMs => replay(trace, { mode: 'source-time', delayMs }))] };
}
if (require.main === module) {
  try {
    if (!process.argv[2]) throw new Error('Usage: node scripts/performance/simulate-frame-selection.cjs trace.csv [new-report.json]');
    const result = JSON.stringify(analyze(parse(fs.readFileSync(process.argv[2], 'utf8'))), null, 2) + '\n';
    if (process.argv[3]) fs.writeFileSync(process.argv[3], result, { flag: 'wx' }); else console.log(result);
  } catch (e) { console.error(e.message); process.exitCode = 1; }
}
module.exports = { parse, verify, replay, analyze };
