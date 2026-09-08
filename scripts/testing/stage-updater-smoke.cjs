// Mutating fixture staging only; does not launch capture, a server, or an installer.
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '../..');
const profile = path.resolve(process.argv[2] || '');
if (path.dirname(profile) !== path.join(root, '.cache') || !path.basename(profile).startsWith('sandbox-probe-')) {
  throw new Error('Provide an existing workspace .cache/sandbox-probe-* profile');
}
const input = path.join(profile, 'input');
if (!fs.existsSync(path.join(input, 'probe-token.txt'))) throw new Error('Missing Sandbox guard token');
const files = [
  [path.join(root, 'src-tauri/target/debug/clipture.exe'), 'updater-harness.exe'],
  [path.join(profile, 'fixture-installer.sig'), 'Tauri.exe.sig'],
  [process.execPath, 'node.exe'],
  [path.join(__dirname, 'updater-server.cjs'), 'updater-server.cjs'],
  [path.join(__dirname, 'sandbox-updater.ps1'), 'updater.ps1'],
];
for (const [source, name] of files) fs.copyFileSync(source, path.join(input, name));
// Tauri bundles a copy with its runtime bundle marker patched to NSS, then
// restores the build-directory executable to UNK. Pin that sole expected
// transformation instead of comparing the installer payload to the raw build.
const controller = fs.readFileSync(path.join(root, 'src-tauri/target/release/clipture.exe'));
const marker = Buffer.from('__TAURI_BUNDLE_TYPE_VAR_UNK');
const offset = controller.indexOf(marker);
if (offset < 0 || offset !== controller.lastIndexOf(marker)) throw new Error('Ambiguous Tauri bundle marker');
Buffer.from('__TAURI_BUNDLE_TYPE_VAR_NSS').copy(controller, offset);
fs.writeFileSync(path.join(input, 'expected-controller.exe'), controller);
const publicKey = fs.readFileSync(path.join(profile, 'fixture-signing.key.pub'), 'utf8').trim();
fs.writeFileSync(path.join(input, 'updater-fixture.json'), JSON.stringify({
  endpoint: 'https://localhost:18443/latest.json', pubkey: publicKey,
}));
console.log('Staged VM-only updater fixture; no production publishing or host installation.');
