'use strict';
// Explicit single-recorder hardware check. Close the daily recorder first.
// Probe profiles, logs and video are confined to a new workspace directory.
const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');
const { probe } = require('./probe-engine-cadence.cjs');
const { summarize } = require('./summarize-pipeline-trace.cjs');
async function main() {
  const executable = path.resolve(process.argv[2] || 'build/engine/Release/clipture_engine.exe');
  const directory = fs.mkdtempSync(path.resolve('.cache/cs2-perf/timing-probe-'));
  const report = { note: 'Sequential uncontrolled desktop, not matched CS2 smoke. CPU and FPS differences are not causal game-load estimates.',
    executable, sha256: createHash('sha256').update(fs.readFileSync(executable)).digest('hex'), trials: [] };
  for (const [label, trace, idle] of [
    ['trace-off', '0', '1'], ['trace-on', '1', '1'],
    ['trace-on-yield', '1', '0'], ['trace-off-repeat', '0', '1']]) {
    const trialDir = path.join(directory, label);
    const trial = await probe(executable, 120, trialDir, 10,
      { CLIPTURE_PIPELINE_TRACE: trace, CLIPTURE_CAPTURE_IDLE_BACKOFF: idle });
    const timing = summarize(fs.readFileSync(path.join(trialDir, 'engine.log'), 'utf8'));
    fs.writeFileSync(path.join(trialDir, 'timing.json'), JSON.stringify(timing, null, 2));
    report.trials.push({ label, ...trial, traceRecords: timing.records.length, rejected: timing.rejected });
    fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify(report, null, 2));
    console.log(JSON.stringify({ label, outputFps: trial.outputFps, distinctFps: trial.distinctFps,
      cpuCores: trial.cpuCoreEquivalents, drops: trial.totalDrops, decode: trial.decode,
      shutdown: trial.shutdown, traceRecords: timing.records.length, rejected: timing.rejected, directory }));
    if (!trial.passedThroughputSmoke || trial.shutdown.watchdogExpired || trial.shutdown.exitCode !== 0 ||
        timing.rejected || (trace === '1' && !timing.records.length)) {
      throw Error('Probe failed; inspect report before deploying.');
    }
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
