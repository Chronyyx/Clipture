'use strict';
// Sequential, isolated capture trials. No settings edits, recorder shutdown,
// synthetic game input, or simultaneous capture engines. Policy is child-only.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { probe } = require('./probe-engine-cadence.cjs');

async function main() {
  if (!process.argv[2]) throw Error('Usage: node probe-capture-pipeline.cjs <candidate.exe> [seconds=10]');
  const executable = path.resolve(process.argv[2]);
  const seconds = Number(process.argv[3] || 10);
  if (!Number.isInteger(seconds) || seconds < 1 || seconds > 60) throw Error('Use 1-60 seconds.');
  const directory = fs.mkdtempSync(path.resolve(__dirname, '../../.cache/cs2-perf/pipeline-'));
  const report = { executable, sha256: crypto.createHash('sha256').update(fs.readFileSync(executable)).digest('hex'),
    note: 'Uncontrolled desktop, sequential one-variable comparisons; not CS2 smoke validation.', trials: [] };
  for (const [label, fps, defer, nv12, isolate, idle] of [
    ['control', 120, 0, 0, 0, 1],
    ['deferred-only', 120, 1, 0, 0, 1],
    ['nv12-only', 120, 0, 1, 0, 1],
    ['deferred-nv12', 120, 1, 1, 0, 1],
    ['isolated', 120, 1, 1, 1, 1],
    ['idle-yield', 120, 1, 1, 0, 0],
    ['repeat-shared', 120, 1, 1, 0, 1],
    ['shared-240', 240, 1, 1, 0, 1],
  ]) {
    const overrides = { CLIPTURE_DEFER_PREPARATION: String(defer), CLIPTURE_NV12_INPUT: String(nv12),
      CLIPTURE_ISOLATE_NVENC: String(isolate), CLIPTURE_CAPTURE_IDLE_BACKOFF: String(idle) };
    console.log(JSON.stringify({ starting: label, directory }));
    const result = await probe(executable, fps, path.join(directory, label), seconds, overrides);
    report.trials.push({ label, overrides, ...result });
    fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify(report, null, 2));
    console.log(JSON.stringify({ label, outputFps: result.outputFps, distinctFps: result.distinctFps,
      cpuCores: result.cpuCoreEquivalents, drops: result.totalDrops, decode: result.decode }));
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
