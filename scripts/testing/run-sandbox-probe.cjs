// Explicit GUI test launcher. Shares only a copied probe and a new output folder.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');
const root = path.resolve(__dirname, '../..');
const profile = fs.mkdtempSync(path.join(root, '.cache/sandbox-probe-'));
const input = path.join(profile, 'input');
const output = path.join(profile, 'output');
fs.mkdirSync(input); fs.mkdirSync(output);
fs.copyFileSync(path.join(__dirname, 'sandbox-probe.ps1'), path.join(input, 'probe.ps1'));
if (process.argv.includes('--installer-inputs')) {
  const files = [
    ['scripts/testing/sandbox-installer.ps1', 'installer.ps1'],
    ['src-tauri/target/release/bundle/nsis/Clipture_1.4.2_x64-setup.exe', 'Tauri.exe'],
    ['.cache/Clipture-Electron-Setup-1.4.2.exe', 'Electron.exe'],
    ['.cache/MicrosoftEdgeWebView2RuntimeInstallerX64.exe', 'WebView2.exe']
  ];
  for (const [source, name] of files) fs.copyFileSync(path.join(root, source), path.join(input, name));
}
const token = crypto.randomUUID();
fs.writeFileSync(path.join(input, 'probe-token.txt'), token);
const xml = value => value.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
const config = path.join(profile, 'probe.wsb');
fs.writeFileSync(config, `<Configuration>
<Networking>Disable</Networking><AudioInput>Disable</AudioInput>
<VideoInput>Disable</VideoInput><ClipboardRedirection>Disable</ClipboardRedirection>
<PrinterRedirection>Disable</PrinterRedirection><MemoryInMB>2048</MemoryInMB>
<MappedFolders>
<MappedFolder><HostFolder>${xml(input)}</HostFolder><SandboxFolder>C:\\CliptureInput</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
<MappedFolder><HostFolder>${xml(output)}</HostFolder><SandboxFolder>C:\\CliptureOutput</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
</MappedFolders>
<LogonCommand><Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\\CliptureInput\\probe.ps1</Command></LogonCommand>
</Configuration>`);
const child = spawn('WindowsSandbox.exe', [config], { windowsHide: true, stdio: 'inherit' });
console.log(JSON.stringify({ profile, sandboxPid: child.pid }));
const started = Date.now();
const timer = setInterval(() => {
  const resultPath = path.join(output, 'probe-result.json');
  if (fs.existsSync(resultPath)) {
    let result;
    try { result = JSON.parse(fs.readFileSync(resultPath, 'utf8').replace(/^\uFEFF/, '')); } catch { return; }
    clearInterval(timer);
    if (result.token !== token) throw new Error('Unexpected Sandbox probe identity');
    console.log(JSON.stringify(result, null, 2));
    // The probe stays open for inspection; never kill a process by image name.
    child.unref(); process.exitCode = result.ok ? 0 : 1;
  } else if (Date.now() - started > 120000) {
    clearInterval(timer); child.unref();
    console.error('Sandbox did not produce its probe report within two minutes.');
    process.exitCode = 1;
  }
}, 500);
child.on('error', error => { clearInterval(timer); console.error(error); process.exitCode = 1; });
child.on('exit', code => {
  if (code && !fs.existsSync(path.join(output, 'probe-result.json'))) {
    clearInterval(timer); console.error(`Sandbox launcher exited with ${code}`); process.exitCode = 1;
  }
});
