'use strict';
// Offline analysis only: sustained output is not the same as unique motion.
const counters = ['encoderOutputPackets', 'encoderDistinctSourceFrames',
  'capturePublishedFrames', 'encoderQueueDrops', 'schedulerDroppedFrames', 'droppedFrames'];

function summarize(trial) {
  let previous = { elapsed: 0, diagnostics: trial.before };
  const windows = [];
  let invalidWindows = 0;
  for (const sample of [...(trial.samples || []), { elapsed: trial.diagnosticElapsed, diagnostics: trial.after }]) {
    const seconds = sample.elapsed - previous.elapsed;
    const delta = Object.fromEntries(counters.map(key => [key,
      sample.diagnostics?.[key] - previous.diagnostics?.[key]]));
    if (!(seconds > 0) || Object.values(delta).some(value => !Number.isFinite(value) || value < 0)) {
      invalidWindows++;
    } else if (seconds >= 0.5) {
      windows.push({ seconds, outputFps: delta.encoderOutputPackets / seconds,
        distinctFps: delta.encoderDistinctSourceFrames / seconds,
        publishedFps: delta.capturePublishedFrames / seconds,
        queueDrops: delta.encoderQueueDrops, schedulerDrops: delta.schedulerDroppedFrames,
        totalDrops: delta.droppedFrames });
    }
    previous = sample;
  }
  return { label: trial.label, targetFps: trial.fps, outputFps: trial.outputFps,
    distinctFps: trial.distinctFps, cpuCores: trial.cpuCoreEquivalents,
    queueDrops: trial.queueDrops, schedulerDrops: trial.schedulerDrops,
    totalDrops: trial.totalDrops, decodeOk: trial.decode?.ok,
    windowCount: windows.length, invalidWindows,
    minimumWindowOutputFps: windows.length ? Math.min(...windows.map(w => w.outputFps)) : null,
    windowsBelow98Percent: windows.filter(w => w.outputFps < trial.fps * 0.98).length,
    windowsWithDrops: windows.filter(w => w.totalDrops || w.queueDrops || w.schedulerDrops).length,
    windows };
}

if (require.main === module) {
  const report = JSON.parse(require('node:fs').readFileSync(process.argv[2], 'utf8'));
  console.log(JSON.stringify(report.trials.map(summarize), null, 2));
}
module.exports = { summarize };
