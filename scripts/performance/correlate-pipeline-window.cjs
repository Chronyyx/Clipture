'use strict';
// Read-only interval correlation; no assumption that any interval is smoke.
const fs = require('node:fs');
const { summarize } = require('./summarize-pipeline-trace.cjs');
function correlate(report, trace, gpu, from, until) {
  if (!Number.isFinite(from) || !Number.isFinite(until) || from >= until) throw Error('Invalid UTC window');
  const rows = report.frameDropAnalysis.timeline.filter(s => !s.reset && !s.captureEpochChanged &&
    s.windowMs > 0 && Date.parse(s.sampledAt) - s.windowMs >= from && Date.parse(s.sampledAt) <= until);
  const ms = rows.reduce((n, s) => n + s.windowMs, 0);
  if (!ms) throw Error('No complete diagnostic windows in requested interval');
  const rate = key => rows.reduce((n, s) => n + s.freshnessRates[key] * s.windowMs, 0) / ms;
  const samples = gpu.samples.filter(s => Date.parse(s.receivedAt) >= from && Date.parse(s.receivedAt) <= until);
  if (!samples.length) throw Error('No aligned GPU samples');
  const vendor = Object.fromEntries(['utilization.gpu', 'utilization.encoder', 'clocks.current.graphics'].map(key => {
    const values = samples.map(s => Number(s[key])).filter(Number.isFinite).sort((a, b) => a - b);
    return [key, { min: values[0], max: values.at(-1), median: values[Math.floor(values.length / 2)], count: values.length }];
  }));
  const timings = summarize(trace, from, until);
  return { from: new Date(from).toISOString(), until: new Date(until).toISOString(),
    note: 'Engine rows are wholly contained intervals; traces select observation-window ends, and GPU samples use received UTC. No causal proof or per-game utilization attribution.',
    engineSeconds: ms / 1000, engineWindows: rows.length,
    fps: Object.fromEntries(['freshFramesPublished', 'distinctSourceFramesEncoded', 'encoderPacketsProduced',
      'desktopUpdateSupply'].map(k => [k, rate(k)])),
    countedReasons: Object.fromEntries(Object.keys(rows[0].reasons).map(k => [k, rows.reduce((n, s) => n + s.reasons[k], 0)])),
    maxSampledP95Ms: Object.fromEntries(['captureAcquireP95_100ns', 'nvencCallP95_100ns',
      'encoderQueueResidenceP95_100ns', 'schedulerWakeLatenessP95_100ns'].map(k => [k, Math.max(...rows.map(s => s.context[k])) / 10000])),
    vendor, timings };
}
if (require.main === module) {
  try {
    const [diagnostic, log, gpu, start, end, output] = process.argv.slice(2);
    if (!output) throw Error('Usage: node correlate-pipeline-window.cjs <export.json> <trace.log> <gpu.json> <from-UTC> <until-UTC> <new-output.json>');
    const read = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
    const result = correlate(read(diagnostic), fs.readFileSync(log, 'utf8'), read(gpu), Date.parse(start), Date.parse(end));
    fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n', { flag: 'wx' });
    console.log(JSON.stringify({ output, engineSeconds: result.engineSeconds, fps: result.fps,
      vendor: result.vendor, rejectedTraces: result.timings.rejected }));
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
module.exports = { correlate };
