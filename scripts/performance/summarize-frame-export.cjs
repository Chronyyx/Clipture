'use strict';
const fs = require('node:fs');

function summarize(report) {
  const history = report?.frameDropAnalysis?.timeline;
  if (!Array.isArray(history)) throw new Error('Expected a Clipture diagnostics export with frameDropAnalysis.timeline.');
  const samples = history.filter(sample => !sample.reset && !sample.captureEpochChanged && sample.windowMs > 0);
  const reasons = {}, classifications = {};
  let seconds = 0, weightedFresh = 0, weightedAccepted = 0, weightedOutput = 0, weightedDistinct = 0;
  for (const sample of samples) {
    const duration = sample.windowMs / 1000;
    seconds += duration;
    const rates = sample.freshnessRates;
    for (const key of ['freshFramesPublished', 'encoderFramesAccepted', 'encoderPacketsProduced', 'distinctSourceFramesEncoded']) {
      if (!Number.isFinite(rates?.[key])) throw new Error(`Missing numeric freshness rate ${key}`);
    }
    weightedFresh += rates.freshFramesPublished * duration;
    weightedAccepted += rates.encoderFramesAccepted * duration;
    weightedOutput += rates.encoderPacketsProduced * duration;
    weightedDistinct += rates.distinctSourceFramesEncoded * duration;
    for (const [key, value] of Object.entries(sample.reasons ?? {})) reasons[key] = (reasons[key] ?? 0) + value;
    const label = sample.visualFreshnessBottleneck ?? 'unknown';
    classifications[label] = (classifications[label] ?? 0) + duration;
  }
  const worst = [...samples].sort((a, b) => b.reportedDropsPerSecond - a.reportedDropsPerSecond ||
    a.freshnessRates.distinctSourceFramesEncoded - b.freshnessRates.distinctSourceFramesEncoded).slice(0, 20);
  return { application: report.application, exportedAt: report.exportedAt,
    note: 'Freshness can fall in static scenes. Classifications are heuristics, not proven root causes. Superseded/coalesced high-refresh source updates are not automatically missed 60 FPS output.',
    samples: samples.length, skippedResetOrTransitionSamples: history.length - samples.length, seconds,
    averageFps: seconds ? { capturePublished: weightedFresh / seconds, encoderAccepted: weightedAccepted / seconds,
      encoded: weightedOutput / seconds, distinctEncoded: weightedDistinct / seconds } : null,
    reasonCounts: reasons, classificationSeconds: classifications,
    worstWindows: worst.map(sample => ({ at: sample.sampledAt, windowMs: sample.windowMs,
      reason: sample.dominantCountedReason, classification: sample.visualFreshnessBottleneck,
      dropsPerSecond: sample.reportedDropsPerSecond, rates: sample.freshnessRates,
      reasons: sample.reasons, timingContext: sample.context })) };
}

if (require.main === module) {
  try {
    if (!process.argv[2] || process.argv[2] === '--help') {
      console.log('node scripts/performance/summarize-frame-export.cjs <export.json> [new-summary.json]');
    } else {
      const result = JSON.stringify(summarize(JSON.parse(fs.readFileSync(process.argv[2], 'utf8').replace(/^\uFEFF/, ''))), null, 2);
      if (process.argv[3]) fs.writeFileSync(process.argv[3], result + '\n', { flag: 'wx' });
      else console.log(result);
    }
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
module.exports = { summarize };
