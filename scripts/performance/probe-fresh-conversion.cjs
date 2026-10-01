'use strict';
// Single-recorder compatibility gate, not a matched CS2/game-performance test.
const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');
const { spawnSync } = require('node:child_process');
const { probe } = require('./probe-engine-cadence.cjs');

function requireNoRecorder() {
  const result = spawnSync('powershell.exe', ['-NoProfile', '-Command',
    "if (Get-Process -Name clipture,clipture_engine -ErrorAction SilentlyContinue) { exit 2 }"],
    { windowsHide: true, encoding: 'utf8' });
  if (result.error || result.status !== 0) throw Error('Exit Clipture first; no recorder is stopped by this probe.');
}

function flags(enabled, extra = {}) {
  return { CLIPTURE_CAPTURE_BACKEND: 'wgc', CLIPTURE_DIRECT_FRESH_CONVERSION: enabled ? '1' : '0',
    CLIPTURE_EARLY_SOURCE_RETIRE: '0', CLIPTURE_DIRECT_TEXTURE_READ: '1',
    CLIPTURE_ISOLATE_NVENC: '1', CLIPTURE_GPU_HANDOFF: '1', CLIPTURE_NV12_INPUT: '1',
    CLIPTURE_DEFER_PREPARATION: '1', CLIPTURE_CAPTURE_IDLE_BACKOFF: '1',
    CLIPTURE_PIPELINE_TRACE: '0', ...extra };
}

async function main() {
  requireNoRecorder();
  const executable = path.resolve('build/engine/Release/clipture_engine.exe');
  const directory = fs.mkdtempSync(path.resolve('.cache/cs2-perf/fresh-conversion-'));
  const matrix = [
    ['control', false, {}, {}], ['candidate', true, {}, {}], ['control-repeat', false, {}, {}],
    ['scaled', true, {}, { targetWidth: 1280, targetHeight: 720 }],
    ['bgra-fallback', true, { CLIPTURE_NV12_INPUT: '0' }, {}],
    ['shared-fallback', true, { CLIPTURE_ISOLATE_NVENC: '0' }, {}],
  ];
  const report = { executable, sha256: createHash('sha256').update(fs.readFileSync(executable)).digest('hex'),
    note: 'Uncontrolled scene; compatibility only. Game FPS and matched smoke improvement not measured.',
    expectedTrials: matrix.length, complete: false, trials: [] };
  const persist = () => fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify(report, null, 2));
  for (const [label, enabled, extra, configuration] of matrix) {
    requireNoRecorder();
    console.log(JSON.stringify({ starting: label, directory }));
    report.activeTrial = label; persist();
    const trialDirectory = path.join(directory, label);
    try {
      const trial = await probe(executable, 120, trialDirectory, 10, flags(enabled, extra), configuration);
      const log = fs.readFileSync(path.join(trialDirectory, 'engine.log'), 'utf8');
      const activated = log.includes('conversion=direct-fresh-nv12');
      const expected = enabled && extra.CLIPTURE_NV12_INPUT !== '0' && extra.CLIPTURE_ISOLATE_NVENC !== '0';
      const priorityUnchanged = /\[encoder\] D3D11 scheduling gpuPriorityRequested=1 .*gpuPriorityActive=1 /.test(log);
      const slotDrops = trial.after.captureSlotDrops - trial.before.captureSlotDrops;
      const passed = trial.passedThroughputSmoke && slotDrops === 0 && activated === expected &&
        priorityUnchanged && !log.includes('source-retirement=after-private-copy') &&
        trial.shutdown.exitCode === 0 && !trial.shutdown.watchdogExpired;
      report.trials.push({ label, ...trial, activated, expected, priorityUnchanged, slotDrops, passed });
      persist();
      console.log(JSON.stringify({ label, passed, activated, priorityUnchanged, outputFps: trial.outputFps,
        distinctFps: trial.distinctFps, slotDrops, report: path.join(directory, 'report.json') }));
      if (!passed) throw Error('Compatibility failed; do not deploy this candidate.');
    } catch (error) { report.error = String(error); persist(); throw error; }
  }
  report.activeTrial = null;
  report.complete = true;
  persist();
}
module.exports = { flags, requireNoRecorder };
if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
