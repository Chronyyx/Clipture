// Raw native WebView2 control experiment; no Clipture services or user profile.
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const root = path.resolve(__dirname, '../..');
const profile = fs.mkdtempSync(path.join(root, '.cache/raw-webview-'));
const cycles = Number(process.argv[2] || 10);
const settleSeconds = Number(process.env.CLIPTURE_RAW_SETTLE_SECONDS || 0);
if (!Number.isInteger(settleSeconds) || settleSeconds < 0 || settleSeconds > 900) throw new Error('Expected 0..900 settling seconds');
if (!Number.isInteger(cycles) || cycles < 1 || cycles > 100) throw new Error('Expected 1..100 cycles');
const child = spawn(path.join(root, 'src-tauri/target/debug/examples/webview_lifecycle.exe'),
  [profile, String(cycles)], { cwd: root, windowsHide: true, stdio: 'inherit' });
const monitor = spawn('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
  path.join(__dirname, 'watch-tauri-resources.ps1'), '-AppProcessId', String(child.pid), '-Profile', profile],
  { windowsHide: true, stdio: 'inherit' });
console.log(JSON.stringify({ profile, pid: child.pid }));
const timer = setTimeout(() => child.kill(), (cycles * 25 + 30 + settleSeconds) * 1000);
child.on('error', error => { clearTimeout(timer); monitor.kill(); console.error(error); process.exitCode = 1; });
monitor.on('error', error => { console.error(error); process.exitCode = 1; });
child.on('exit', async code => {
  clearTimeout(timer);
  const orphanCheck = await require('./finish-resource-monitor.cjs')(monitor, profile);
  const file = path.join(profile, 'raw-webview-result.json');
  if (!fs.existsSync(file)) { console.error(`No raw result; exit=${code}`); process.exitCode=1; return; }
  const rows = JSON.parse(fs.readFileSync(file, 'utf8'));
  const counts = rows.map(row => row.closed.controllerHandleTypes?.Process ?? 0);
  const growth = counts.at(-1) - counts[0];
  const ok = code === 0 && rows.length === cycles && orphanCheck.ok &&
    rows.every(row => row.open.ok && row.closed.ok) && growth <= 2;
  const settledFile = path.join(profile, 'raw-webview-settled.json');
  const settled = fs.existsSync(settledFile) ? JSON.parse(fs.readFileSync(settledFile, 'utf8')) : null;
  const report = { ok, cycles, retainedProcessHandles: counts, growth, orphanCheck, settled, rows };
  fs.writeFileSync(path.join(profile, 'control-result.json'), JSON.stringify(report, null, 2));
  console.log(JSON.stringify({ ok, cycles, retainedProcessHandles: counts, growth, orphanCheck, settled, profile }, null, 2));
  if (!ok) process.exitCode=1;
});
