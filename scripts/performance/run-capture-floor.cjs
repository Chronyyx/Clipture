'use strict';
// What capture alone costs the game: runs the stress window alone, then while
// clipture_capture_floor holds a bare WGC session on it (frames discarded: no
// copy, tone map or encode), and reports window fps lost and presentation
// modes per phase. Same window options as run-stress-trial.cjs for the CS2 setup.
// Usage: node scripts/performance/run-capture-floor.cjs --label <name> [--monitor] [--fp16] [--interval-ms N]
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { windowCost } = require('./stress/score-stress-trial.cjs');

const root = path.resolve(__dirname, '../..');
const release = path.join(root, 'build/engine/Release');
const TIMELINE = 'clear:3:0,smoke:10:6,clear:2:0,molly:10:10,clear:2:0,spike:4:20,clear:2:0';

const args = process.argv.slice(2);
const flag = name => args.includes(name);
const value = (name, fallback) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : fallback; };
const label = value('--label', 'floor');
const stamp = new Date().toISOString().replace(/[:.]/g, '-');
const directory = path.join(root, '.cache/stress-trials', `${stamp}-${label}`);
fs.mkdirSync(directory, { recursive: true });

const gameDirectory = path.join(root, '.cache/stress-game');
fs.mkdirSync(gameDirectory, { recursive: true });
const windowExe = path.join(gameDirectory, 'cs2.exe');
fs.copyFileSync(path.join(release, 'clipture_stress_window.exe'), windowExe);
const windowArgs = out => ['--timeline', TIMELINE, '--out', out, '--reflex', '--render-size', '1920x1440',
  '--compute-load', '--cpu-threads', '12', '--cpu-busy', '100', '--cpu-games-priority',
  ...(value('--layer-ms') ? ['--layer-ms', value('--layer-ms')] : [])];

const run = (exe, argv, input) => new Promise((resolve, reject) => {
  const child = spawn(exe, argv, { stdio: ['pipe', 'pipe', 'pipe'] });
  let out = '', err = '';
  child.stdout.on('data', d => { out += d; input?.(String(d), child); });
  child.stderr.on('data', d => { err += d; });
  child.on('exit', code => code === 0 ? resolve(out) : reject(Error(`${path.basename(exe)} exited ${code}: ${err}`)));
  return child;
});

async function main() {
  await run(windowExe, [...windowArgs(path.join(directory, 'solo')), '--autostart']);
  const solo = JSON.parse(fs.readFileSync(path.join(directory, 'solo/summary.json'), 'utf8'));
  // The window waits for "start"; the floor session attaches first.
  const window = spawn(windowExe, [...windowArgs(path.join(directory, 'trial')), '--layer-ms', String(solo.layerMs)],
    { stdio: ['pipe', 'pipe', 'pipe'] });
  const windowExit = new Promise(resolve => window.on('exit', resolve));
  await new Promise(resolve => window.stdout.on('data', d => { if (String(d).includes('"ready"')) resolve(); }));
  const floor = spawn(path.join(release, 'clipture_capture_floor.exe'),
    [...(flag('--monitor') ? ['--monitor'] : ['--process', 'cs2.exe']), ...(flag('--fp16') ? ['--fp16'] : []),
      ...(value('--interval-ms') ? ['--interval-ms', value('--interval-ms')] : [])], { stdio: ['pipe', 'pipe', 'pipe'] });
  let floorOut = '';
  floor.stdout.on('data', d => { floorOut += d; });
  await new Promise(resolve => { const t = setInterval(() => { if (floorOut.includes('capturing')) { clearInterval(t); resolve(); } }, 50); });
  await new Promise(resolve => setTimeout(resolve, 1500));
  window.stdin.write('start\n');
  const seconds = TIMELINE.split(',').reduce((a, e) => a + Number(e.split(':')[1]), 0) + 1;
  await new Promise(resolve => setTimeout(resolve, seconds * 1000));
  window.stdin.end();
  await windowExit;
  floor.stdin.end();
  await new Promise(resolve => floor.on('exit', resolve));
  const trial = JSON.parse(fs.readFileSync(path.join(directory, 'trial/summary.json'), 'utf8'));
  const modes = summary => Object.fromEntries(summary.phases.filter(p => p.name === 'smoke').map(p => ['smoke', p.presentationModes]));
  const report = { label, monitor: flag('--monitor'), fp16: flag('--fp16'), layerMs: solo.layerMs,
    windowCost: windowCost(solo, trial), soloModes: modes(solo), trialModes: modes(trial),
    foregroundPct: Math.min(...trial.phases.filter(p => p.frames > 10).map(p => p.foregroundPct)), floor: floorOut.trim() };
  fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify(report, null, 2));
  console.log(JSON.stringify({ label, cost: report.windowCost.filter(c => c.name !== 'clear').map(c => `${c.name} ${c.costPct}%`),
    solo: report.soloModes, trial: report.trialModes, fg: report.foregroundPct }));
}

main().catch(error => { console.error(error.message || error); process.exitCode = 1; });
