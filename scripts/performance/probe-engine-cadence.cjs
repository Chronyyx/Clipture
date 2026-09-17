'use strict';
// Explicit hardware probe: primary display, no audio, isolated workspace output.
// Never reads/writes daily settings or restarts an existing recorder.
const fs = require('node:fs');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');
const readline = require('node:readline');
const root = path.resolve(__dirname, '../..');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

async function probe(executable, fps, directory, seconds = 10, overrides = {}, configuration = {}) {
  const startedAt = new Date().toISOString();
  fs.mkdirSync(directory);
  const child = spawn(executable, [], { cwd: directory, windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'],
    env: { ...process.env, ...overrides, TEMP: directory, TMP: directory, APPDATA: directory, LOCALAPPDATA: directory } });
  const pending = new Map();
  let id = 0, stderr = '', result, watchdogExpired = false, terminalError = null;
  const logLimit = overrides.CLIPTURE_PIPELINE_TRACE === '1' ? 1024 * 1024 : 16000;
  child.stderr.on('data', data => { stderr = (stderr + data).slice(-logLimit); });
  child.stdin.on('error', () => {});
  const done = new Promise((resolve, reject) => {
    child.once('exit', code => {
      terminalError = Error(`Engine exited (${code})`);
      for (const p of pending.values()) p.reject(terminalError);
      pending.clear();
      resolve(code);
    });
    child.once('error', error => {
      terminalError = error;
      for (const p of pending.values()) p.reject(error);
      pending.clear();
      resolve(null);
    });
  });
  const watchdog = setTimeout(() => {
    watchdogExpired = true;
    terminalError = Error('Engine probe watchdog expired');
    for (const p of pending.values()) p.reject(terminalError);
    pending.clear();
    child.kill();
  }, (seconds + 40) * 1000);
  const lines = readline.createInterface({ input: child.stdout });
  lines.on('line', line => {
    try {
      const message = JSON.parse(line), item = pending.get(message.id);
      if (!item) return;
      pending.delete(message.id);
      if (message.error) item.reject(Error(message.error)); else item.resolve(message.payload);
    } catch { /* unrelated output cannot satisfy a request */ }
  });
  const request = fields => new Promise((resolve, reject) => {
    if (terminalError || child.exitCode !== null || child.signalCode !== null) {
      reject(terminalError || Error('Engine is no longer running'));
      return;
    }
    const nextId = ++id;
    pending.set(nextId, { resolve, reject });
    child.stdin.write(JSON.stringify({ id: nextId, ...fields }) + '\n');
  });
  const cpu = () => {
    const p = spawnSync('powershell.exe', ['-NoProfile', '-Command',
      `(Get-Process -Id ${child.pid} -ErrorAction Stop).TotalProcessorTime.TotalSeconds; [Diagnostics.Stopwatch]::GetTimestamp(); [Diagnostics.Stopwatch]::Frequency`],
    { encoding: 'utf8', windowsHide: true });
    const values = p.stdout.trim().split(/\s+/).map(Number);
    if (p.status !== 0 || values.length !== 3 || values.some(v => !Number.isFinite(v))) throw Error('CPU sample failed');
    return { cpu: values[0], time: values[1] / values[2] };
  };
  try {
    await request({ type: 'configure', fps, bitrateMbps: 50, nvencPreset: 3,
      clipLengthSeconds: 120, monitorId: 'primary', targetWidth: 0, targetHeight: 0,
      includeMixedAudio: false, includeSystemAudio: false, includeMicrophoneAudio: false,
      captureGameAudio: false, captureForegroundSystemAudio: false,
      ...configuration,
      saveInPlace: true, saveFolder: directory });
    await delay(2500);
    const before = await request({ type: 'getDiagnostics' });
    const diagnosticStart = performance.now();
    const measuredFrom = new Date().toISOString();
    if (!before.captureReady || !before.engineRunning) throw Error(before.status || 'Capture not ready');
    const cpuStart = cpu();
    const samples = [];
    for (let second = 0; second < seconds; ++second) {
      await delay(1000);
      const diagnostics = await request({ type: 'getDiagnostics' });
      samples.push({ elapsed: (performance.now() - diagnosticStart) / 1000, diagnostics });
    }
    const cpuEnd = cpu(), elapsed = cpuEnd.time - cpuStart.time;
    const after = await request({ type: 'getDiagnostics' });
    const diagnosticElapsed = (performance.now() - diagnosticStart) / 1000;
    result = { executable, startedAt, measuredFrom, measuredUntil: new Date().toISOString(), fps, elapsed, diagnosticElapsed, cpuCoreEquivalents: (cpuEnd.cpu - cpuStart.cpu) / elapsed,
      before, after, samples, save: await request({ type: 'saveClip', durationSeconds: 3, saveFolder: directory }) };
    if (!result.save.ok) throw Error(result.save.message);
    const decoded = spawnSync(path.join(root, 'node_modules/ffmpeg-static/ffmpeg.exe'),
      ['-v', 'error', '-nostdin', '-threads', '2', '-i', result.save.clip.filePath, '-an', '-f', 'null', '-'],
      { windowsHide: true, encoding: 'utf8', timeout: 20000 });
    result.decode = { ok: decoded.status === 0, error: decoded.stderr };
    result.outputFps = (after.encoderOutputPackets - before.encoderOutputPackets) / diagnosticElapsed;
    result.queueDrops = after.encoderQueueDrops - before.encoderQueueDrops;
    result.schedulerDrops = after.schedulerDroppedFrames - before.schedulerDroppedFrames;
    result.totalDrops = after.droppedFrames - before.droppedFrames;
    result.distinctFps = (after.encoderDistinctSourceFrames - before.encoderDistinctSourceFrames) / diagnosticElapsed;
    result.passedThroughputSmoke = result.decode.ok && result.outputFps >= fps * 0.98 &&
      result.queueDrops === 0 && result.schedulerDrops === 0 && result.totalDrops === 0;
    return result;
  } finally {
    const stopStarted = performance.now();
    child.stdin.end();
    const exitCode = await done;
    if (result) result.shutdown = { exitCode, watchdogExpired, elapsedMs: performance.now() - stopStarted };
    clearTimeout(watchdog);
    fs.writeFileSync(path.join(directory, 'engine.log'), stderr);
  }
}

async function main() {
  if (!process.argv[2] || !process.argv[3]) throw Error('Usage: node probe-engine-cadence.cjs <baseline.exe> <candidate.exe> [candidate-fps-list=120,144] [seconds=10]');
  const rates = (process.argv[4] || '120,144').split(',').map(Number);
  const seconds = Number(process.argv[5] || 10);
  if (!rates.length || rates.length > 7 || rates.some(rate => ![24,30,60,120,144,210,240].includes(rate)) ||
      new Set(rates).size !== rates.length || !Number.isInteger(seconds) || seconds < 1 || seconds > 60) {
    throw Error('Use unique supported frame rates and 1-60 seconds per trial.');
  }
  const directory = fs.mkdtempSync(path.join(root, '.cache/cs2-perf/cadence-probe-'));
  const trials = [];
  for (const [label, executable, fps] of [
    ['baseline-120', process.argv[2], 120],
    ...rates.map(fps => [`candidate-${fps}`, process.argv[3], fps])]) {
    const trial = await probe(path.resolve(executable), fps, path.join(directory, label), seconds);
    trials.push({ label, ...trial });
    fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify({
      note: 'Sequential uncontrolled desktop probe; no game-load or motion-specific throughput claim.', trials
    }, null, 2));
    console.log(JSON.stringify({ label, cpuCores: trial.cpuCoreEquivalents,
      acquired: trial.after.captureAcquiredUpdates - trial.before.captureAcquiredUpdates,
      polls: trial.after.captureAcquireTimeouts - trial.before.captureAcquireTimeouts,
      encoded: trial.after.encoderOutputPackets - trial.before.encoderOutputPackets,
      drops: trial.after.droppedFrames - trial.before.droppedFrames,
      outputFps: trial.outputFps, distinctFps: trial.distinctFps, schedulerDrops: trial.schedulerDrops,
      passedThroughputSmoke: trial.passedThroughputSmoke,
      decode: trial.decode, report: path.join(directory, 'report.json') }));
  }
}
module.exports = { probe };
if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
