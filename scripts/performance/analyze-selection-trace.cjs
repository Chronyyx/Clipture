#!/usr/bin/env node
// Explains every repeated output frame in a selection trace recorded with
// CLIPTURE_SELECTION_TRACE_PATH (engine events: a = capture arrival, s =
// recorded-source switch, v = coverage change, p = queued frame, t/l = on-time
// and late encoder ticks). Read-only.
//
// A tick that takes no new frame repeats the previous one. Its cause is:
//   none-delivered  no frame from the recorded source was presented for its interval
//   late-delivery   a frame for its interval arrived only after the tick selected
//   not-queued      a frame arrived in time but was not queued before the tick
// Usage: node scripts/performance/analyze-selection-trace.cjs <trace.csv> [--seconds]
'use strict';
const fs = require('fs');

function parse(file) {
  const text = fs.readFileSync(file, 'utf8');
  const header = JSON.parse((text.match(/^# (.*)$/m) || [, '{}'])[1]);
  const events = [];
  for (const line of text.split('\n')) {
    if (!line || line.startsWith('#') || line.startsWith('kind')) continue;
    const [kind, at, source, deadline, epoch, sequence, depth, coalesced] = line.split(',');
    events.push({ kind, at: +at, source: +source, deadline: +deadline, epoch: +epoch,
      sequence: +sequence, depth: +depth, coalesced: +coalesced });
  }
  return { header, events };
}

function percentile(values, q) {
  if (!values.length) return null;
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))];
}

function analyze({ header, events }) {
  const ms = value => +(value / 1e4).toFixed(2);
  const arrivals = events.filter(e => e.kind === 'a');
  const recorded = arrivals.filter(e => e.depth & 2);
  const pushes = events.filter(e => e.kind === 'p');
  const ticks = events.filter(e => (e.kind === 't' || e.kind === 'l') && e.deadline > 0)
    .sort((a, b) => a.at - b.at);
  const switches = events.filter(e => e.kind === 's');
  const coverage = events.filter(e => e.kind === 'v');

  // Sort once; scan with moving indices.
  const byPts = [...recorded].sort((a, b) => a.source - b.source);
  const pushesByPts = [...pushes].sort((a, b) => a.source - b.source);
  const inRange = (list, lo, hi) => {
    let low = 0, high = list.length;
    while (low < high) { const mid = (low + high) >> 1; if (list[mid].source <= lo) low = mid + 1; else high = mid; }
    const out = [];
    for (let i = low; i < list.length && list[i].source <= hi; i++) out.push(list[i]);
    return out;
  };

  const causes = { 'none-delivered': 0, 'late-delivery': 0, 'not-queued': 0 };
  const repeats = [];
  let previousDeadline = 0;
  for (const tick of ticks) {
    const took = tick.kind === 't' ? tick.depth > 0 : tick.sequence > 0;
    if (previousDeadline && !took) {
      const due = inRange(byPts, previousDeadline, tick.deadline);
      let cause;
      if (!due.length) cause = 'none-delivered';
      else if (due.every(a => a.at > tick.at)) cause = 'late-delivery';
      else cause = 'not-queued';
      causes[cause]++;
      const queued = inRange(pushesByPts, previousDeadline, tick.deadline);
      repeats.push({ at: tick.at, deadline: tick.deadline, kind: tick.kind, cause, due: due.length,
        firstArrivalAfterTickMs: due.length ? ms(Math.min(...due.map(a => a.at)) - tick.at) : null,
        queuedLateMs: queued.length ? ms(Math.min(...queued.map(p => p.at)) - tick.at) : null });
    }
    previousDeadline = tick.deadline;
  }

  // Delivery: how long after presentation each recorded frame arrived, and
  // the longest gap between recorded-source frames.
  const latency = recorded.map(a => (a.at - a.source) / 1e4).filter(v => v > -50 && v < 1000);
  const gaps = [];
  const recordedByAt = [...recorded].sort((a, b) => a.at - b.at);
  for (let i = 1; i < recordedByAt.length; i++) gaps.push((recordedByAt[i].at - recordedByAt[i - 1].at) / 1e4);
  const tickLate = ticks.map(t => (t.at - t.deadline) / 1e4);

  // Per second: arrivals by source, recorded source, ticks and repeats.
  const start = events.length ? Math.min(...events.map(e => e.at)) : 0;
  const seconds = new Map();
  const bucket = at => {
    const key = Math.floor((at - start) / 1e7);
    if (!seconds.has(key)) seconds.set(key, { second: key, game: 0, desktop: 0, recordedArrivals: 0,
      queued: 0, ticks: 0, repeats: 0, none: 0, late: 0, notQueued: 0, maxGapMs: 0 });
    return seconds.get(key);
  };
  for (const a of arrivals) {
    const b = bucket(a.at);
    if (a.depth & 1) b.game++; else b.desktop++;
    if (a.depth & 2) b.recordedArrivals++;
  }
  for (const p of pushes) bucket(p.at).queued++;
  for (const t of ticks) bucket(t.at).ticks++;
  for (const r of repeats) {
    const b = bucket(r.at);
    b.repeats++;
    if (r.cause === 'none-delivered') b.none++;
    else if (r.cause === 'late-delivery') b.late++;
    else b.notQueued++;
  }
  for (let i = 1; i < recordedByAt.length; i++) {
    const b = bucket(recordedByAt[i].at);
    b.maxGapMs = Math.max(b.maxGapMs, +((recordedByAt[i].at - recordedByAt[i - 1].at) / 1e4).toFixed(1));
  }
  let recordingGame = null;
  const sourceAt = at => {
    let state = null;
    for (const s of switches) { if (s.at <= at) state = s.depth ? 'game' : 'desktop'; else break; }
    return state;
  };
  for (const b of seconds.values()) b.recording = sourceAt(start + (b.second + 1) * 1e7) || 'desktop';
  recordingGame = switches.length ? switches[switches.length - 1].depth === 1 : false;

  return {
    header,
    events: events.length,
    lost: header.lost,
    durationS: +(((events.length ? Math.max(...events.map(e => e.at)) : 0) - start) / 1e7).toFixed(1),
    ticks: ticks.length,
    repeats: repeats.length,
    causes,
    switches: switches.map(s => ({ atS: +((s.at - start) / 1e7).toFixed(2), to: s.depth ? 'game' : 'desktop' })),
    coverage: coverage.map(c => ({ atS: +((c.at - start) / 1e7).toFixed(2),
      reason: ['covered', 'overlay', 'waiting for game frames', 'size mismatch', 'other'][c.depth] || c.depth })),
    recordingGameAtEnd: recordingGame,
    deliveryLatencyMs: { p50: percentile(latency, .5), p90: percentile(latency, .9), p99: percentile(latency, .99), max: percentile(latency, 1) },
    recordedGapMs: { p50: percentile(gaps, .5), p99: percentile(gaps, .99), max: percentile(gaps, 1) },
    tickLatenessMs: { p50: percentile(tickLate, .5), p99: percentile(tickLate, .99), max: percentile(tickLate, 1) },
    perSecond: [...seconds.values()].sort((a, b) => a.second - b.second),
    repeatsSample: repeats.slice(0, 40).map(r => ({ ...r, atS: +((r.at - start) / 1e7).toFixed(3) })),
  };
}

if (require.main === module) {
  const file = process.argv[2];
  if (!file) { console.error('Usage: analyze-selection-trace.cjs <trace.csv> [--seconds]'); process.exit(2); }
  const result = analyze(parse(file));
  const { perSecond, repeatsSample, ...summary } = result;
  console.log(JSON.stringify(summary, null, 2));
  if (process.argv.includes('--seconds')) {
    console.table(perSecond.filter(s => s.repeats > 0 || s.maxGapMs > 30));
    console.table(repeatsSample);
  }
}

module.exports = { parse, analyze };
