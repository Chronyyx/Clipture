'use strict';
// Scores decoded clip stamps against the stress window's frame log.
//
// Relative lag is clip time minus present (submit) time, shifted so the
// fastest frame is 0 ms; its tail shows images that reached the recording late.
//
// A repeat is an output frame showing the same window frame as the one before
// it. When the window runs near the capture rate, sampling alone produces
// repeats, so a repeat only counts as missed when the next window frame had
// been submitted longer ago than the expected path to the recording: the
// unloaded-phase median lag, plus one window frame (its own GPU queue), plus
// one display refresh (the compositor only shows it on the next refresh).
//
// Stutter counts every repeat in a phase where the window, and the display
// refresh that limits what reaches the desktop, outran the capture rate by 50%.
// Below that the window's own frame-time jitter can leave a tick without a new
// frame; above it a new frame always existed, so each repeat is a visible hitch
// whatever caused it (including frames taken early by encoder catch-up).

const percentile = (values, fraction) => {
  if (!values.length) return null;
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.floor(fraction * (sorted.length - 1))];
};
const round = value => value === null ? null : Math.round(value * 100) / 100;

function parseFrameLog(csv) {
  const frames = new Map();
  for (const line of csv.split(/\r?\n/).slice(1)) {
    if (!line) continue;
    const [counter, phase, presentUs] = line.split(',').map(Number);
    frames.set(counter, { phase, presentUs });
  }
  return frames;
}

function scoreTrial({ stamps, frames, summary, captureFps }) {
  const rows = [];
  let previous = null;
  for (const stamp of stamps) {
    const record = stamp.counter === null ? undefined : frames.get(stamp.counter);
    if (!record) { rows.push({ decoded: false }); previous = null; continue; }
    rows.push({ decoded: true, counter: stamp.counter, phase: record.phase,
      repeat: previous !== null && stamp.counter === previous,
      backwards: previous !== null && stamp.counter < previous,
      lagUs: stamp.ptsSeconds * 1e6 - record.presentUs });
    previous = stamp.counter;
  }
  const decoded = rows.filter(row => row.decoded);
  const lagOffset = decoded.length ? Math.min(...decoded.map(row => row.lagUs)) : 0;
  const frameMs = 1000 / captureFps;

  const relativeLagMs = row => (row.lagUs - lagOffset) / 1000;
  const unloaded = new Set(summary.phases.filter(phase => phase.layers === 0).map(phase => phase.index));
  const baselineLagMs = percentile(decoded.filter(row => unloaded.has(row.phase)).map(relativeLagMs), 0.5) ?? 0;

  const phases = [];
  for (const phase of summary.phases) {
    const own = [];
    for (let i = 0; i < rows.length; ++i) if (rows[i].decoded && rows[i].phase === phase.index) own.push(i);
    if (!own.length) continue;
    const windowFrameMs = phase.fps > 0 ? 1000 / phase.fps : 0;
    const missAfterMs = baselineLagMs + windowFrameMs + 1000 / (summary.refreshHz || 60);
    let missed = 0;
    for (const i of own) {
      const next = rows[i].repeat ? frames.get(rows[i].counter + 1) : undefined;
      if (next && (stamps[i].ptsSeconds * 1e6 - lagOffset - next.presentUs) / 1000 > missAfterMs) ++missed;
    }
    let longestRun = 1, run = 1;
    for (let k = 1; k < own.length; ++k) {
      run = rows[own[k]].counter === rows[own[k - 1]].counter ? run + 1 : 1;
      longestRun = Math.max(longestRun, run);
    }
    const lags = own.map(i => relativeLagMs(rows[i]));
    phases.push({ index: phase.index, name: phase.name, layers: phase.layers, windowFps: round(phase.fps),
      outputFrames: own.length, uniqueFrames: new Set(own.map(i => rows[i].counter)).size,
      repeats: own.filter(i => rows[i].repeat).length, missedFrames: missed,
      stutterFrames: Math.min(phase.fps, summary.refreshHz || Infinity) >= 1.5 * captureFps ? own.filter(i => rows[i].repeat).length : 0,
      freshnessPct: round(100 * (1 - missed / own.length)),
      backwards: own.filter(i => rows[i].backwards).length,
      longestFreezeMs: round(longestRun * frameMs),
      lagMs: { p50: round(percentile(lags, 0.5)), p95: round(percentile(lags, 0.95)),
        p99: round(percentile(lags, 0.99)), max: round(percentile(lags, 1)) } });
  }
  const timed = phases.filter(phase => phase.name !== 'lead' && phase.name !== 'tail');
  const sum = key => timed.reduce((total, phase) => total + phase[key], 0);
  const loaded = timed.filter(phase => phase.layers > 0);
  return {
    captureFps, outputFrames: rows.length, undecodedFrames: rows.length - decoded.length,
    baselineLagMs: round(baselineLagMs),
    totals: { outputFrames: sum('outputFrames'), stutterFrames: sum('stutterFrames'), missedFrames: sum('missedFrames'),
      freshnessPct: round(100 * (1 - sum('missedFrames') / Math.max(1, sum('outputFrames')))),
      loadedMissedFrames: loaded.reduce((total, phase) => total + phase.missedFrames, 0),
      worstFreezeMs: timed.length ? Math.max(...timed.map(phase => phase.longestFreezeMs)) : null,
      worstLagP99Ms: timed.length ? Math.max(...timed.map(phase => phase.lagMs.p99)) : null },
    phases,
  };
}

// Window frame-rate cost of capture, per phase, from solo and trial summaries.
function windowCost(solo, trial) {
  return trial.phases.filter(phase => phase.frames > 1).map(phase => {
    const alone = solo.phases.find(candidate => candidate.index === phase.index);
    return { index: phase.index, name: phase.name, soloFps: round(alone?.fps ?? null), trialFps: round(phase.fps),
      costPct: alone?.fps ? round(100 * (1 - phase.fps / alone.fps)) : null };
  });
}

function compareReports(baseline, candidate) {
  const delta = (a, b) => (a === null || b === null || a === undefined || b === undefined) ? null : round(b - a);
  return {
    stutterFrames: delta(baseline.score.totals.stutterFrames, candidate.score.totals.stutterFrames),
    missedFrames: delta(baseline.score.totals.missedFrames, candidate.score.totals.missedFrames),
    loadedMissedFrames: delta(baseline.score.totals.loadedMissedFrames, candidate.score.totals.loadedMissedFrames),
    freshnessPct: delta(baseline.score.totals.freshnessPct, candidate.score.totals.freshnessPct),
    worstFreezeMs: delta(baseline.score.totals.worstFreezeMs, candidate.score.totals.worstFreezeMs),
    worstLagP99Ms: delta(baseline.score.totals.worstLagP99Ms, candidate.score.totals.worstLagP99Ms),
    phases: candidate.score.phases.map(phase => {
      const base = baseline.score.phases.find(other => other.index === phase.index);
      const baseCost = baseline.windowCost.find(other => other.index === phase.index);
      const cost = candidate.windowCost.find(other => other.index === phase.index);
      return { index: phase.index, name: phase.name,
        missedFrames: delta(base?.missedFrames, phase.missedFrames),
        lagP99Ms: delta(base?.lagMs.p99, phase.lagMs.p99),
        windowCostPct: delta(baseCost?.costPct, cost?.costPct) };
    }),
  };
}

module.exports = { parseFrameLog, scoreTrial, windowCost, compareReports };
