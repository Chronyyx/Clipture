'use strict';
// Copy only the runtime payload, never Cargo intermediates or Electron output.
const fs = require('node:fs');
const path = require('node:path');
const { assertStandaloneRuntime } = require('./release/windows-runtime-imports.cjs');
const root = path.resolve(__dirname, '..');
const source = path.join(root, 'src-tauri/target/release');
const output = path.join(root, 'release/tauri-unpacked');
const files = ['clipture.exe', 'clipture_engine.exe', 'ffmpeg.exe',
  'assets/default.mp3', 'assets/option2.wav'];

function stage() {
  // Validate the complete input before modifying an existing staged app.
  for (const file of files) {
    const input = path.join(source, file);
    if (!fs.statSync(input).isFile()) throw new Error(`Missing runtime file: ${input}`);
    if (file.endsWith('.exe')) assertStandaloneRuntime(input);
  }
  for (const file of files) {
    const destination = path.join(output, file);
    fs.mkdirSync(path.dirname(destination), { recursive: true });
    fs.copyFileSync(path.join(source, file), destination);
  }
  console.log(`Tauri runtime staged at ${output} (${files.length} files; no Node or Electron).`);
}

if (require.main === module) stage();
module.exports = { files, output };
