'use strict';
// Prolonged recording check: an isolated engine records for N minutes while
// the stress window replays a 5-minute load timeline, then saves and decodes
// the last two minutes. Reports drops, the clip and engine counters; pair with
// a process resource sampler for leak checks.
// Usage: node scripts/performance/run-long-recording.cjs <engine.exe> [minutes=10] [fps=60]
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { probe } = require('./probe-engine-cadence.cjs');

const root = path.resolve(__dirname, '../..');
const release = path.join(root, 'build/engine/Release');
const engine = path.resolve(process.argv[2]);
const minutes = Number(process.argv[3] || 10);
const fps = Number(process.argv[4] || 60);
const TIMELINE = 'clear:20:0,smoke:60:6,clear:20:0,molly:60:10,clear:20:0,spike:20:20,clear:40:0,smoke:60:6';

function dailyAudio() {
  const settings = JSON.parse(fs.readFileSync(path.join(process.env.APPDATA, 'Clipture/data/settings.json'), 'utf8'));
  const sources = settings.audioSources || [];
  const mic = sources.find(source => source.kind === 'microphone');
  return { includeMicrophoneAudio: !!mic?.enabled, captureGameAudio: sources.some(s => s.id === 'game' && s.enabled),
    micVolume: mic?.volume ?? 1, micIsolation: mic?.voiceIsolation ?? false, noiseGateEnabled: mic?.noiseGateEnabled ?? true,
    autoNoiseGate: mic?.autoNoiseGate ?? true, micDeviceId: mic?.micDeviceId ?? '' };
}

async function main() {
  const stamp = new Date().toISOString().replace(/[:.]/g, '-');
  const directory = path.join(root, '.cache/stress-trials', `${stamp}-long-${path.basename(engine, '.exe')}`);
  fs.mkdirSync(directory, { recursive: true });
  const gameDirectory = path.join(root, '.cache/stress-game');
  fs.mkdirSync(gameDirectory, { recursive: true });
  const windowExe = path.join(gameDirectory, 'cs2.exe');
  fs.copyFileSync(path.join(release, 'clipture_stress_window.exe'), windowExe);
  let stopWindows = false;
  const windows = (async () => {
    for (let round = 0; !stopWindows; ++round) {
      const child = spawn(windowExe, ['--timeline', TIMELINE, '--out', path.join(directory, `window-${round}`), '--autostart',
        '--reflex', '--render-size', '1920x1440', '--compute-load', '--layer-ms', '0.2'], { stdio: ['pipe', 'ignore', 'ignore'] });
      await new Promise(resolve => child.on('exit', resolve));
    }
  })();
  await new Promise(resolve => setTimeout(resolve, 3000));
  const result = await probe(engine, fps, path.join(directory, 'engine'), minutes * 60, {}, dailyAudio(), { saveSeconds: 120 });
  stopWindows = true;
  const delta = key => (result.after[key] ?? 0) - (result.before[key] ?? 0);
  const report = { engine, minutes, fps, save: { ok: result.save.ok, durationSeconds: result.save.clip?.durationSeconds,
      audioTracks: (result.save.clip?.audioTracks || []).length }, decode: result.decode,
    outputFps: result.outputFps, distinctFps: result.distinctFps, drops: result.totalDrops,
    queueDrops: result.queueDrops, schedulerDrops: result.schedulerDrops, cpuCores: result.cpuCoreEquivalents,
    captureSlotDrops: delta('captureSlotDrops'), encoderRestarts: delta('encoderSessionRestarts'),
    gpuClass: result.after.gpuSchedulingClass, shutdown: result.shutdown };
  fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify(report, null, 2));
  console.log(JSON.stringify(report));
  await windows;
}

main().catch(error => { console.error(error.message || error); process.exitCode = 1; });
