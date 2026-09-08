// Explicit hardware test: records the primary display, without audio, into a
// new workspace profile. Never modifies daily Clipture settings or startup.
const fs = require('node:fs');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '../..');
fs.mkdirSync(path.join(root, '.cache'), { recursive: true });
const profile = fs.mkdtempSync(path.join(root, '.cache/tauri-capture-'));
const clips = path.join(profile, 'clips');
fs.mkdirSync(clips);
const ffmpeg = path.join(root, 'node_modules/ffmpeg-static/ffmpeg.exe');
fs.writeFileSync(path.join(profile, 'settings.json'), JSON.stringify({
  saveFolder: clips, startOnLogin: false, hotkey: 'Ctrl+Alt+Shift+F24',
  fps: 30, resolutionPreset: '360p', autoBitrate: false, bitrateMbps: 4,
  clipLengthSeconds: 10, clipSound: 'none', showNotification: true,
  audioSources: ['system', 'mic', 'game'].map(id => ({ id, label: id,
    kind: id === 'mic' ? 'microphone' : id, enabled: false }))
}));
fs.writeFileSync(path.join(profile, 'clips.json'), '[]');
const executable = path.resolve(process.argv[2] || path.join(root, 'src-tauri/target/debug/clipture.exe'));
const child = spawn(executable, ['--capture-smoke', '--hidden'], { cwd: root, windowsHide: true,
  env: { ...process.env, CLIPTURE_TEST_MODE: '0', CLIPTURE_DATA_DIR: profile,
    CLIPTURE_SMOKE_RESOURCES: '1', CLIPTURE_ENGINE_PATH: path.join(root, 'build/engine/Release/clipture_engine.exe'),
    CLIPTURE_FFMPEG_PATH: ffmpeg }, stdio: 'inherit' });
const monitor = spawn('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
  path.join(__dirname, 'watch-tauri-resources.ps1'), '-AppProcessId', String(child.pid), '-Profile', profile],
  { windowsHide: true, stdio: 'inherit' });
monitor.on('error', error => console.error('Resource monitor failed:', error));
const timer = setTimeout(() => child.kill(), 200000);
child.on('error', error => { clearTimeout(timer); monitor.kill(); throw error; });
child.on('exit', async code => {
  clearTimeout(timer);
  const orphanCheck = await require('./finish-resource-monitor.cjs')(monitor, profile);
  const reportFile = path.join(profile, 'capture-smoke-result.json');
  if (!fs.existsSync(reportFile)) { console.error(`No capture report: exit=${code}; profile=${profile}`); process.exitCode = 1; return; }
  const report = JSON.parse(fs.readFileSync(reportFile, 'utf8'));
  report.orphanCheck = orphanCheck;
  report.ok = report.ok && orphanCheck.ok;
  if (report.ok) {
    report.decodedClips = (report.clips || []).map(clip => {
      const decoded = spawnSync(ffmpeg, ['-nostdin', '-v', 'error', '-i', clip.filePath,
        '-frames:v', '5', '-f', 'null', '-'], { windowsHide: true, encoding: 'utf8' });
      return { id: clip.id, ok: decoded.status === 0, error: decoded.stderr };
    });
    report.ok = report.decodedClips.length === 3 && report.decodedClips.every(clip => clip.ok);
    fs.writeFileSync(reportFile, JSON.stringify(report, null, 2));
  }
  fs.writeFileSync(reportFile, JSON.stringify(report, null, 2));
  console.log(JSON.stringify({ ok: report.ok, checks: report.checks, error: report.error, orphanCheck,
    decodedClips: report.decodedClips, reportFile }, null, 2));
  if (!report.ok) process.exitCode = 1;
});
