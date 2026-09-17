'use strict';
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { probe } = require('./probe-engine-cadence.cjs');
async function main() {
  if (!process.argv[2]) throw Error('Usage: node probe-pipeline-compatibility.cjs <candidate.exe>');
  const executable = path.resolve(process.argv[2]);
  const directory = fs.mkdtempSync(path.resolve(__dirname, '../../.cache/cs2-perf/pipeline-compat-'));
  const report = { executable, sha256: crypto.createHash('sha256').update(fs.readFileSync(executable)).digest('hex'),
    note: 'Isolated desktop compatibility/decode tests, not a CS2 smoke test.', trials: [] };
  for (const [label, seconds, overrides, configuration] of [
    ['default-120', 30, {}, {}],
    ['scaled-nv12', 10, {}, { targetWidth: 1280, targetHeight: 720 }],
    ['scaled-bgra', 10, { CLIPTURE_NV12_INPUT: '0' }, { targetWidth: 1280, targetHeight: 720 }],
    ['legacy-keyed', 10, { CLIPTURE_GPU_HANDOFF: '0' }, {}],
    ['shared-device', 10, { CLIPTURE_ISOLATE_NVENC: '0' }, {}],
  ]) {
    console.log(JSON.stringify({ starting: label, directory }));
    const result = await probe(executable, 120, path.join(directory, label), seconds, overrides, configuration);
    report.trials.push({ label, overrides, configuration, ...result });
    fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify(report, null, 2));
    console.log(JSON.stringify({ label, outputFps: result.outputFps, drops: result.totalDrops,
      decode: result.decode, status: result.after.status }));
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
