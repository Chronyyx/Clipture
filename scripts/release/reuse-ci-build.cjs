// Stands in for `tauri build` when release.yml publishes the build CI made
// for the same commit. tauri-action runs this as its build command, then
// finds the installer and updater signature at their usual paths, creates
// the draft release and writes latest.json exactly as after a real build.
// Fails unless every file a release needs is present for this version.
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '../..');
const version = require('../../src-tauri/tauri.conf.json').version;
const release = path.join(root, 'src-tauri/target/x86_64-pc-windows-msvc/release');
const installer = path.join(release, 'bundle/nsis', `Clipture_${version}_x64-setup.exe`);
const required = [
  path.join(release, 'clipture.exe'),
  installer,
  `${installer}.sig`,
  path.join(root, 'src-tauri/binaries/clipture_engine-x86_64-pc-windows-msvc.exe'),
  path.join(root, 'src-tauri/binaries/ffmpeg-x86_64-pc-windows-msvc.exe'),
  path.join(root, 'dist/renderer/index.html'),
];
const missing = required.filter((file) => !fs.existsSync(file) || fs.statSync(file).size === 0);
if (missing.length) {
  console.error(`The CI build is incomplete for ${version}:\n${missing.map((file) => `  ${path.relative(root, file)}`).join('\n')}`);
  process.exit(1);
}
const installers = fs.readdirSync(path.dirname(installer)).filter((name) => name.endsWith('.exe'));
if (installers.length !== 1) {
  console.error(`Expected one installer for ${version}, found: ${installers.join(', ')}`);
  process.exit(1);
}
console.log(`Reusing the CI build of ${version}: ${path.relative(root, installer)}`);
