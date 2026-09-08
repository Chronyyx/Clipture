'use strict';
const { existsSync } = require('node:fs');
const { join } = require('node:path');
const { spawn } = require('node:child_process');
const { files, output } = require('./stage-tauri-app.cjs');

function launch(args = process.argv.slice(2), spawnApp = spawn, fileExists = existsSync) {
  for (const file of files) {
    if (!fileExists(join(output, file))) throw new Error(`Missing ${file}; run npm run build first.`);
  }
  // The launcher exits after spawning. Node/npm must not remain at tray idle.
  // Preserve explicit isolated-profile environment overrides for testing.
  const child = spawnApp(join(output, 'clipture.exe'), args, {
    cwd: output, env: process.env, detached: true, windowsHide: true, stdio: 'ignore',
  });
  child.once('error', error => { console.error(`Unable to launch Tauri: ${error.message}`); process.exitCode = 1; });
  child.once('spawn', () => console.log(`Started Tauri Clipture (PID ${child.pid}).`));
  child.unref();
  return child;
}

if (require.main === module) launch();
module.exports = { launch };
