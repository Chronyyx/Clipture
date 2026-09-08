// Runs the actual debug Tauri executable against generated media and an
// isolated profile. Never reads or writes the daily Clipture profile.
const { mkdirSync, mkdtempSync, writeFileSync, readFileSync, existsSync } = require('node:fs');
const { resolve, join } = require('node:path');
const { spawn, spawnSync } = require('node:child_process');
const root = resolve(__dirname, '../..');
const cache = join(root, '.cache');
mkdirSync(cache, { recursive: true });
const profile = mkdtempSync(join(cache, 'tauri-smoke-'));
const clips = join(profile, 'clips');
mkdirSync(clips);
const video = join(clips, 'fixture.mp4');
const ffmpeg = join(root, 'node_modules/ffmpeg-static/ffmpeg.exe');
const generated = spawnSync(ffmpeg, ['-nostdin', '-hide_banner', '-loglevel', 'error',
  '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=24',
  '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
  '-f', 'lavfi', '-i', 'sine=frequency=660:sample_rate=48000',
  '-t', '3', '-map', '0:v', '-map', '1:a', '-map', '2:a',
  '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-movflags', '+faststart', video],
  { windowsHide: true, encoding: 'utf8' });
if (generated.status !== 0) throw new Error(generated.stderr);
writeFileSync(join(profile, 'settings.json'), JSON.stringify({ saveFolder: clips, startOnLogin: false }));
writeFileSync(join(profile, 'clips.json'), JSON.stringify([{ id: 'smoke-fixture', title: 'Smoke fixture',
  gameOrApp: 'Fixture', createdAt: '2026-09-04T00:00:00.000Z', durationSeconds: 3,
  filePath: video, resolution: '320x180', fps: 24, encoder: 'Software', audioTracks: ['System', 'Microphone'] }]));
const executable = resolve(process.argv[2] || join(root, 'src-tauri/target/debug/clipture.exe'));
const cycles = Number(process.env.CLIPTURE_SMOKE_CYCLES || 3);
if (!Number.isInteger(cycles) || cycles < 1 || cycles > 100) throw new Error('CLIPTURE_SMOKE_CYCLES must be 1–100');
const child = spawn(executable, ['--smoke-test'], { cwd: root, windowsHide: true,
  env: { ...process.env, CLIPTURE_TEST_MODE: '1', CLIPTURE_DATA_DIR: profile,
    CLIPTURE_SMOKE_CYCLES: String(cycles), CLIPTURE_SMOKE_RESOURCES: '1',
    CLIPTURE_FFMPEG_PATH: ffmpeg }, stdio: 'inherit' });
const monitor = spawn('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass',
  '-File', join(__dirname, 'watch-tauri-resources.ps1'), '-AppProcessId', String(child.pid), '-Profile', profile],
  { windowsHide: true, stdio: 'inherit' });
monitor.on('error', error => console.error('Resource monitor failed:', error));
const timer = setTimeout(() => child.kill(), (75 + cycles * 40) * 1000);
child.on('error', error => { clearTimeout(timer); throw error; });
child.on('exit', async code => {
  clearTimeout(timer);
  const orphanCheck = await require('./finish-resource-monitor.cjs')(monitor, profile);
  const reportFile = join(profile, 'smoke-result.json');
  if (!existsSync(reportFile)) { console.error(`No smoke report; exit=${code}; profile=${profile}`); process.exitCode = 1; return; }
  const report = JSON.parse(readFileSync(reportFile, 'utf8'));
  report.orphanCheck = orphanCheck;
  report.ok = report.ok && orphanCheck.ok;
  const processHandles = sample => Object.entries(sample?.controllerHandleTypes || {})
    .filter(([type]) => type === 'Process' || type.startsWith('Process:'))
    .reduce((sum, [, count]) => sum + count, 0);
  if (report.cycles >= 10) {
    const first = report.reports[0].resources.closed;
    const last = report.reports.at(-1).resources.closed;
    report.retainedProcessHandleGrowth = processHandles(last) - processHandles(first);
    report.resourceGatePassed = report.retainedProcessHandleGrowth <= 2;
    report.ok = report.ok && report.resourceGatePassed;
    writeFileSync(reportFile, JSON.stringify(report, null, 2));
  }
  writeFileSync(reportFile, JSON.stringify(report, null, 2));
  console.log(JSON.stringify({ ok: report.ok, cycles: report.cycles, orphanCheck,
    resourceGatePassed: report.resourceGatePassed, retainedProcessHandleGrowth: report.retainedProcessHandleGrowth,
    noWebviewsAfterClose: report.noWebviewsAfterClose, error: report.error,
    first: report.reports?.[0], last: report.reports?.at(-1), reportFile }, null, 2));
  if (!report.ok) process.exitCode = 1;
});
