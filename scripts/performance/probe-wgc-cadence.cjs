'use strict';
// Explicit single-recorder desktop compatibility, not proof of CS2 performance.
const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');
const { probe } = require('./probe-engine-cadence.cjs');
async function main() {
  const executable = path.resolve('build/engine/Release/clipture_engine.exe');
  const retirement = process.argv.includes('--early-retire');
  const early = { CLIPTURE_EARLY_SOURCE_RETIRE: '1' };
  const trials = retirement ? [
    ['wgc-control', 'wgc', 120, {}, {}],
    ['wgc-private-copy', 'wgc', 120, early, {}],
    ['wgc-control-repeat', 'wgc', 120, {}, {}],
    ['wgc-private-trace', 'wgc', 120, { ...early, CLIPTURE_PIPELINE_TRACE: '1' }, {}],
    ['dxgi-private-copy', 'dxgi', 120, early, {}],
    ['wgc-scaled-private', 'wgc', 120, early, { targetWidth: 1280, targetHeight: 720 }],
    ['wgc-bgra-fallback', 'wgc', 120, { ...early, CLIPTURE_NV12_INPUT: '0' }, {}],
    ['wgc-shared-fallback', 'wgc', 120, { ...early, CLIPTURE_ISOLATE_NVENC: '0' }, {}],
    ['wgc-private-144', 'wgc', 144, early, {}],
  ] : [
    ['dxgi-120', 'dxgi', 120, {}, {}],
    ['dxgi-bgra', 'dxgi', 120, { CLIPTURE_NV12_INPUT: '0' }, {}],
    ['wgc-120', 'wgc', 120, {}, {}],
    ['wgc-trace', 'wgc', 120, { CLIPTURE_PIPELINE_TRACE: '1' }, {}],
    ['wgc-144', 'wgc', 144, {}, {}],
    ['wgc-scaled', 'wgc', 120, {}, { targetWidth: 1280, targetHeight: 720 }],
    ['wgc-bgra', 'wgc', 120, { CLIPTURE_NV12_INPUT: '0' }, {}],
    ['wgc-eager', 'wgc', 120, { CLIPTURE_DEFER_PREPARATION: '0' }, {}],
    ['wgc-shared-device', 'wgc', 120, { CLIPTURE_ISOLATE_NVENC: '0' }, {}],
  ];
  const directory = fs.mkdtempSync(path.resolve(`.cache/cs2-perf/${retirement ? 'source-retirement' : 'wgc-cadence'}-`));
  const report = { executable, sha256: createHash('sha256').update(fs.readFileSync(executable)).digest('hex'),
    note: 'Uncontrolled desktop; no game-FPS or smoke improvement claim.', complete: false, expectedTrials: trials.length, trials: [] };
  const persist = () => fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify(report, null, 2));
  for (const [label, backend, fps, overrides, configuration] of trials) {
    console.log(JSON.stringify({ starting: label, directory }));
    report.activeTrial = label;
    persist();
    const trialDir = path.join(directory, label);
    let result;
    try { result = await probe(executable, fps, trialDir, 10, {
      CLIPTURE_CAPTURE_BACKEND: backend, CLIPTURE_PIPELINE_TRACE: '0',
      CLIPTURE_DIRECT_TEXTURE_READ: '1', CLIPTURE_ISOLATE_NVENC: '1',
      CLIPTURE_GPU_HANDOFF: '1', CLIPTURE_NV12_INPUT: '1',
      CLIPTURE_EARLY_SOURCE_RETIRE: '0',
      CLIPTURE_DEFER_PREPARATION: '1', ...overrides,
    }, configuration); } catch (error) {
      report.error = String(error);
      persist();
      throw error;
    }
    const log = fs.readFileSync(path.join(trialDir, 'engine.log'), 'utf8');
    const direct = log.includes('handoff=direct-texture-read producerWait=false');
    const slots = result.after.captureSlotDrops - result.before.captureSlotDrops;
    const expectedDirect = overrides.CLIPTURE_ISOLATE_NVENC !== '0';
    const earlyActive = log.includes('source-retirement=after-private-copy before-conversion=true');
    const expectedEarly = overrides.CLIPTURE_EARLY_SOURCE_RETIRE === '1' &&
      expectedDirect && overrides.CLIPTURE_NV12_INPUT !== '0';
    const backendActive = backend === 'wgc' ? log.includes('Windows.Graphics.Capture started')
      : log.includes('DXGI Desktop Duplication started');
    const passed = result.passedThroughputSmoke && slots === 0 && backendActive && direct === expectedDirect &&
      result.shutdown.exitCode === 0 && !result.shutdown.watchdogExpired && earlyActive === expectedEarly;
    report.trials.push({ label, ...result, slots, direct, earlyActive, backendActive, passed });
    persist();
    console.log(JSON.stringify({ label, passed, fps: result.outputFps, distinct: result.distinctFps,
      drops: result.totalDrops, slots, backend: result.after.activeCaptureBackend, direct, shutdown: result.shutdown }));
    if (!passed) throw Error('Capture compatibility failed; inspect before deployment.');
  }
  report.complete = report.trials.length === report.expectedTrials && report.trials.every(t => t.passed);
  report.activeTrial = null;
  persist();
}
main().catch(error => { console.error(error); process.exitCode = 1; });
