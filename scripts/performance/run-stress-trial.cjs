'use strict';
// Frame-stability trial without a game: runs the stress window alone, then
// again while an isolated engine records it, and scores the saved clip.
// Never touches daily settings or a running Clipture; output goes to .cache/.
const fs = require('node:fs');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');
const readline = require('node:readline');
const { probe } = require('./probe-engine-cadence.cjs');
const { readFrameStamps } = require('./stress/frame-stamps.cjs');
const { parseFrameLog, scoreTrial, windowCost, compareReports } = require('./stress/score-stress-trial.cjs');

const root = path.resolve(__dirname, '../..');
const release = path.join(root, 'build/engine/Release');
const DEFAULT_TIMELINE = 'clear:4:0,smoke:6:6,clear:3:0,molly:4:10,clear:2:0,spike:1:20,clear:3:0';
const USAGE = `Usage: node scripts/performance/run-stress-trial.cjs [options]
  --label <name>        report folder suffix (default: trial)
  --engine <exe>        engine under test (default: build/engine/Release/clipture_engine.exe)
  --fps <n>             capture frame rate (default: 120)
  --timeline <spec>     name:seconds:loadMs,... (default: ${DEFAULT_TIMELINE})
  --hdr                 present FP16 scRGB with HDR highlights
  --render-size <WxH>   render smaller and stretch to the screen (e.g. 1920x1440 4:3)
  --cpu-threads <n>     CPU-burning threads during GPU-loaded phases (game simulation)
  --cpu-busy <pct>      busy share of each CPU thread (default 100)
  --gpu-priority <n>    window D3D11 GPU thread priority -7..7 (games may raise theirs)
  --cpu-games-priority  register CPU threads as MMCSS "Games" (high), like game engine threads
  --compute-load        issue GPU load as compute dispatches (volumetric smoke style)
  --audio               play soft noise from the window during GPU-loaded phases
  --as-game             run the window as cs2.exe so game detection and game-audio capture bind to it
  --daily-audio         capture audio like the daily app (sources from %APPDATA%\Clipture\data\settings.json)
  --env KEY=VALUE       engine environment override; repeatable
  --layer-ms <ms>       pin GPU load calibration (default: baseline's, else measured)
  --baseline <report>   compare against an earlier report.json`;

function parseArgs(argv) {
  const options = { label: 'trial', engine: path.join(release, 'clipture_engine.exe'), fps: 120,
    timeline: DEFAULT_TIMELINE, hdr: false, renderSize: null, cpuThreads: 0, cpuBusy: 100, computeLoad: false, dailyAudio: false, audio: false, asGame: false, cpuGamesPriority: false, gpuPriority: 0, env: {}, layerMs: 0, baseline: null };
  for (let i = 0; i < argv.length; ++i) {
    const arg = argv[i], value = () => { if (i + 1 >= argv.length) throw Error(USAGE); return argv[++i]; };
    if (arg === '--label') options.label = value();
    else if (arg === '--engine') options.engine = path.resolve(value());
    else if (arg === '--fps') options.fps = Number(value());
    else if (arg === '--timeline') options.timeline = value();
    else if (arg === '--hdr') options.hdr = true;
    else if (arg === '--render-size') options.renderSize = value();
    else if (arg === '--cpu-threads') options.cpuThreads = Number(value());
    else if (arg === '--cpu-busy') options.cpuBusy = Number(value());
    else if (arg === '--compute-load') options.computeLoad = true;
    else if (arg === '--daily-audio') options.dailyAudio = true;
    else if (arg === '--audio') options.audio = true;
    else if (arg === '--as-game') options.asGame = true;
    else if (arg === '--cpu-games-priority') options.cpuGamesPriority = true;
    else if (arg === '--gpu-priority') options.gpuPriority = Number(value());
    else if (arg === '--env') { const [key, ...rest] = value().split('='); options.env[key] = rest.join('='); }
    else if (arg === '--layer-ms') options.layerMs = Number(value());
    else if (arg === '--baseline') options.baseline = path.resolve(value());
    else throw Error(USAGE);
  }
  if (![30, 60, 120, 144, 240].includes(options.fps)) throw Error('Use --fps 30, 60, 120, 144 or 240.');
  if (!/^[\w-]+$/.test(options.label)) throw Error('Labels may use letters, digits, - and _.');
  return options;
}

// Audio part of the app's engine configuration (mirrors EngineConfigure::from_settings).
function dailyAudioConfiguration() {
  const settings = JSON.parse(fs.readFileSync(path.join(process.env.APPDATA, 'Clipture/data/settings.json'), 'utf8'));
  const sources = settings.audioSources || [];
  const system = sources.find(source => source.id === 'system');
  const mic = sources.find(source => source.kind === 'microphone');
  const allSystem = system?.enabled && (system.captureAllSystem ?? true);
  const join = names => [...new Set(names.filter(Boolean))].join('|');
  return {
    includeMixedAudio: false, includeSystemAudio: !!allSystem, includeMicrophoneAudio: !!mic?.enabled,
    captureGameAudio: sources.some(source => source.id === 'game' && source.enabled),
    captureForegroundSystemAudio: !!(system?.enabled && !allSystem),
    appAudioProcesses: join(sources.filter(source => source.kind === 'app' && source.enabled).map(source => source.processName)),
    systemAudioProcesses: join(system?.enabled && !allSystem ? system.processNames || [] : []),
    micVolume: mic?.volume ?? 1, micIsolation: mic?.voiceIsolation ?? false,
    micIsolationWeight: mic?.voiceIsolationWeight ?? 1, noiseGateEnabled: mic?.noiseGateEnabled ?? true,
    autoNoiseGate: mic?.autoNoiseGate ?? true, noiseGateThreshold: mic?.noiseGateThreshold ?? 0.05,
    noiseGateDebounceMs: mic?.noiseGateDebounceMs ?? 180, micDeviceId: mic?.micDeviceId ?? '',
    micDeviceMatchKey: mic?.micDeviceMatchKey ?? '', micDeviceName: mic?.micDeviceName ?? '',
  };
}

const timelineSeconds = spec => spec.split(',').reduce((total, entry) => total + Number(entry.split(':')[1]), 0);

let windowExecutable = path.join(release, 'clipture_stress_window.exe');

function startWindow(args) {
  const child = spawn(windowExecutable, args,
    { windowsHide: false, stdio: ['pipe', 'pipe', 'pipe'] });
  let stderr = '';
  child.stderr.on('data', data => { stderr += data; });
  const events = [];
  const waiters = [];
  readline.createInterface({ input: child.stdout }).on('line', line => {
    try { events.push(JSON.parse(line)); } catch { return; }
    for (const waiter of waiters.splice(0)) waiter();
  });
  const exited = new Promise(resolve => child.once('exit', code => {
    for (const waiter of waiters.splice(0)) waiter();
    resolve(code);
  }));
  const waitFor = async name => {
    while (!events.some(event => event.event === name)) {
      if (child.exitCode !== null) throw Error(`Stress window exited before '${name}': ${stderr}`);
      await new Promise(resolve => waiters.push(resolve));
    }
    return events.find(event => event.event === name);
  };
  return { child, exited, waitFor, stderr: () => stderr };
}

function otherEngines() {
  const list = spawnSync('tasklist', ['/FI', 'IMAGENAME eq clipture_engine*', '/FO', 'CSV', '/NH'], { encoding: 'utf8' });
  return (list.stdout || '').split(/\r?\n/).filter(line => line.startsWith('"clipture'));
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const baseline = options.baseline ? JSON.parse(fs.readFileSync(options.baseline, 'utf8')) : null;
  if (!options.layerMs && baseline) options.layerMs = baseline.layerMs;
  if (otherEngines().length) {
    console.warn('Another Clipture engine is running; it competes for the GPU. Close Clipture for clean numbers.');
  }
  const stamp = new Date().toISOString().replace(/[:.]/g, '-');
  const directory = path.join(root, '.cache/stress-trials', `${stamp}-${options.label}`);
  fs.mkdirSync(directory, { recursive: true });
  if (options.asGame) {
    const gameDirectory = path.join(root, '.cache/stress-game');
    fs.mkdirSync(gameDirectory, { recursive: true });
    windowExecutable = path.join(gameDirectory, 'cs2.exe');
    fs.copyFileSync(path.join(release, 'clipture_stress_window.exe'), windowExecutable);
  }
  const windowArgs = out => ['--timeline', options.timeline, '--out', out,
    ...(options.layerMs ? ['--layer-ms', String(options.layerMs)] : []), ...(options.hdr ? ['--hdr'] : []),
    ...(options.renderSize ? ['--render-size', options.renderSize] : []),
    ...(options.cpuThreads ? ['--cpu-threads', String(options.cpuThreads), '--cpu-busy', String(options.cpuBusy)] : []),
    ...(options.computeLoad ? ['--compute-load'] : []), ...(options.audio ? ['--audio'] : []),
    ...(options.cpuGamesPriority ? ['--cpu-games-priority'] : []),
    ...(options.gpuPriority ? ['--gpu-priority', String(options.gpuPriority)] : [])];

  console.log(JSON.stringify({ step: 'solo', directory }));
  const solo = startWindow([...windowArgs(path.join(directory, 'solo')), '--autostart']);
  if (await solo.exited !== 0) throw Error(`Solo stress window failed: ${solo.stderr()}`);
  const soloSummary = JSON.parse(fs.readFileSync(path.join(directory, 'solo/summary.json'), 'utf8'));
  options.layerMs = soloSummary.layerMs;

  console.log(JSON.stringify({ step: 'trial', layerMs: options.layerMs }));
  const trialWindow = startWindow(windowArgs(path.join(directory, 'trial')));
  let engine;
  try {
    await trialWindow.waitFor('ready');
    const seconds = Math.ceil(timelineSeconds(options.timeline)) + 1;
    engine = await probe(options.engine, options.fps, path.join(directory, 'engine'), seconds, options.env,
      options.dailyAudio ? dailyAudioConfiguration() : {},
      { onMeasureStart: () => trialWindow.child.stdin.write('start\n'), saveSeconds: seconds + 2 });
  } finally {
    trialWindow.child.stdin.end();
    await trialWindow.exited;
  }
  const trialSummary = JSON.parse(fs.readFileSync(path.join(directory, 'trial/summary.json'), 'utf8'));
  const frames = parseFrameLog(fs.readFileSync(path.join(directory, 'trial/frames.csv'), 'utf8'));

  console.log(JSON.stringify({ step: 'score', clip: engine.save.clip.filePath }));
  const { video, stamps } = readFrameStamps(engine.save.clip.filePath,
    trialSummary.bufferWidth ?? trialSummary.width, trialSummary.bufferHeight ?? trialSummary.height);
  // GPU slot pools: slots held at the end, the most held at once (the probe's
  // engine is fresh, so over the whole run), and slots freed for sitting idle.
  const slotPool = (field, releases) => engine.after[field] === undefined ? null : {
    held: engine.after[field], peak: engine.after[`${field}Peak`],
    idleReleases: (engine.after[releases] ?? 0) - (engine.before[releases] ?? 0) };
  const report = {
    label: options.label, startedAt: engine.startedAt, engine: options.engine, env: options.env,
    fps: options.fps, timeline: options.timeline, hdrWindow: options.hdr, renderSize: options.renderSize,
    cpuThreads: options.cpuThreads, cpuBusy: options.cpuBusy, computeLoad: options.computeLoad, dailyAudio: options.dailyAudio, audio: options.audio, asGame: options.asGame, cpuGamesPriority: options.cpuGamesPriority, gpuPriority: options.gpuPriority, layerMs: options.layerMs,
    hdrTonemapping: engine.after.hdrTonemapping ?? null, video,
    slotPools: { capture: slotPool('captureTextureSlots', 'captureTextureSlotIdleReleases'),
      nvenc: slotPool('nvencOutputSlots', 'nvencOutputSlotIdleReleases') },
    // Slots held each second of the measurement: [elapsed s, capture, NVENC].
    slotTimeline: engine.samples.map(({ elapsed, diagnostics }) => [Math.round(elapsed),
      diagnostics.captureTextureSlots ?? null, diagnostics.nvencOutputSlots ?? null]),
    engineCounters: { outputFps: engine.outputFps, distinctFps: engine.distinctFps, totalDrops: engine.totalDrops,
      queueDrops: engine.queueDrops, schedulerDrops: engine.schedulerDrops, cpuCores: engine.cpuCoreEquivalents },
    // Every numeric engine counter that moved during measurement.
    engineDeltas: Object.fromEntries(Object.entries(engine.after)
      .filter(([key, value]) => typeof value === 'number' && typeof engine.before[key] === 'number' &&
        value !== engine.before[key])
      .map(([key, value]) => [key, value - engine.before[key]])),
    score: scoreTrial({ stamps, frames, summary: trialSummary, captureFps: options.fps }),
    windowCost: windowCost(soloSummary, trialSummary),
  };
  if (baseline) report.versusBaseline = compareReports(baseline, report);
  fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify(report, null, 2));
  fs.writeFileSync(path.join(directory, 'stamps.json'), JSON.stringify(stamps));
  fs.rmSync(engine.save.clip.filePath, { force: true });

  console.table(report.score.phases.map(phase => ({ phase: `${phase.index}:${phase.name}`, layers: phase.layers,
    windowFps: phase.windowFps, stutter: phase.stutterFrames, missed: phase.missedFrames, fresh: phase.freshnessPct,
    freezeMs: phase.longestFreezeMs, lagP99: phase.lagMs.p99,
    fg: trialSummary.phases.find(item => item.index === phase.index)?.foregroundPct,
    costPct: report.windowCost.find(cost => cost.index === phase.index)?.costPct })));
  console.log(JSON.stringify({ totals: report.score.totals, undecoded: report.score.undecodedFrames,
    hdrTonemapping: report.hdrTonemapping, slotPools: report.slotPools, versusBaseline: report.versusBaseline && {
      stutterFrames: report.versusBaseline.stutterFrames, missedFrames: report.versusBaseline.missedFrames, worstLagP99Ms: report.versusBaseline.worstLagP99Ms },
    report: path.join(directory, 'report.json') }));
  if (report.score.undecodedFrames > report.score.outputFrames * 0.01) {
    console.warn('More than 1% of frames had no readable stamp; check the window covered the captured monitor.');
  }
}

main().catch(error => { console.error(error.message || error); process.exitCode = 1; });
