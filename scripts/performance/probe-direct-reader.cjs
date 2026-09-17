'use strict';
// Single-recorder desktop compatibility check, not a matched CS2 benchmark.
const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');
const { probe } = require('./probe-engine-cadence.cjs');
async function main() {
  const executable = path.resolve(process.argv[2] || 'build/engine/Release/clipture_engine.exe');
  const directory = fs.mkdtempSync(path.resolve('.cache/cs2-perf/direct-reader-'));
  const report = { executable, sha256: createHash('sha256').update(fs.readFileSync(executable)).digest('hex'),
    note: 'Uncontrolled desktop compatibility only; distinct rates are not a CS2 comparison.', trials: [] };
  for (const [label, fps, overrides, configuration, direct] of [
    ['bridge-120', 120, { CLIPTURE_DIRECT_TEXTURE_READ: '0' }, {}, false],
    ['direct-120', 120, {}, {}, true],
    ['direct-trace', 120, { CLIPTURE_PIPELINE_TRACE: '1' }, {}, true],
    ['scaled-nv12', 120, {}, { targetWidth: 1280, targetHeight: 720 }, true],
    ['scaled-bgra', 120, { CLIPTURE_NV12_INPUT: '0' }, { targetWidth: 1280, targetHeight: 720 }, true],
    ['legacy-keyed', 120, { CLIPTURE_GPU_HANDOFF: '0' }, {}, false],
    ['shared-device', 120, { CLIPTURE_ISOLATE_NVENC: '0' }, {}, false],
    ['direct-144', 144, {}, {}, true],
    ['direct-210', 210, {}, {}, true],
    ['direct-240', 240, {}, {}, true],
  ]) {
    console.log(JSON.stringify({ starting: label, directory }));
    const trialDir = path.join(directory, label);
    const result = await probe(executable, fps, trialDir, 10, {
      CLIPTURE_PIPELINE_TRACE: '0', CLIPTURE_DIRECT_TEXTURE_READ: '1',
      CLIPTURE_CAPTURE_IDLE_BACKOFF: '1', CLIPTURE_ISOLATE_NVENC: '1',
      CLIPTURE_GPU_HANDOFF: '1', CLIPTURE_NV12_INPUT: '1',
      CLIPTURE_DEFER_PREPARATION: '1', ...overrides,
    }, configuration);
    const log = fs.readFileSync(path.join(trialDir, 'engine.log'), 'utf8');
    const directActive = log.includes('handoff=direct-texture-read producerWait=false');
    const captureSlotDrops = result.after.captureSlotDrops - result.before.captureSlotDrops;
    const passed = result.passedThroughputSmoke && result.shutdown.exitCode === 0 &&
      !result.shutdown.watchdogExpired && captureSlotDrops === 0 && directActive === direct;
    report.trials.push({ label, ...result, captureSlotDrops, directActive, passed });
    fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify(report, null, 2));
    console.log(JSON.stringify({ label, passed, outputFps: result.outputFps, distinctFps: result.distinctFps,
      drops: result.totalDrops, captureSlotDrops, decode: result.decode, shutdown: result.shutdown, directActive }));
    if (!passed) throw Error('Direct-reader probe failed; inspect report before deployment.');
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
