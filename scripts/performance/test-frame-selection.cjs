'use strict';
const assert = require('node:assert/strict');
const { parse, verify, replay, analyze } = require('./simulate-frame-selection.cjs');
function fixture(bursty = false) {
  const frames = [], ticks = [];
  for (let i = 1; i <= 1200; ++i) {
    const source = Math.floor(i * 1e7 / 120);
    frames.push({ kind: 'p', at: bursty ? Math.ceil(i / 2) * Math.floor(2e7 / 120) + 5000 : source, source, epoch: 1, sequence: i });
    ticks.push({ kind: 't', at: source + 1000, deadline: source, fps: 120 });
  }
  const events = [...frames, ...ticks].sort((a, b) => a.at - b.at);
  const queue = [];
  for (const e of events) {
    Object.assign(e, { depth: queue.length, coalesced: 0, overflow: 0 });
    if (e.kind === 'p') queue.push(e);
    else { Object.assign(e, { epoch: queue.at(-1)?.epoch || 0, sequence: queue.at(-1)?.sequence || 0, coalesced: Math.max(0, queue.length - 1) }); queue.length = 0; }
  }
  return { meta: { version: 1, complete: true, lost: 0, queueCapacity: 8 }, events };
}
const stable = fixture(); assert.equal(verify(stable), 120);
assert.equal(replay(stable, {}).repeats, 0);
const bursts = fixture(true);
assert.ok(replay(bursts, {}).repeats > 400);
const buffered = replay(bursts, { mode: 'source-time', delayMs: 1000 / 120 });
assert.equal(buffered.repeats, 0); assert.ok(buffered.sourceAgeMs.p95 > 8);
assert.ok(buffered.maximumRetainedQueueDepth <= 3);
assert.equal(analyze(bursts).variants.length, 7);
const wrong = structuredClone(stable); wrong.events.find(e => e.kind === 't').sequence = 900;
assert.throws(() => verify(wrong), /does not replay/);
const epoch = structuredClone(stable); epoch.events.find(e => e.kind === 'p').epoch = 2;
assert.throws(() => verify(epoch));
const partial = '# {"version":1,"complete":false,"lost":0}\n'; assert.throws(() => parse(partial), /Partial/);
assert.throws(() => parse('# {"version":1,"complete":true,"lost":1}\n'), /Lost/);
console.log('Selection replay: uniform/bursty arrivals, bounded latency, exact baseline, transitions, and incomplete/lost trace rejection passed.');
