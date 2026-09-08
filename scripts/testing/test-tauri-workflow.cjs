'use strict';
// No app launch, filesystem writes, engine or daily profile access.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { EventEmitter } = require('node:events');
const root = path.resolve(__dirname, '../..');
const scripts = require('../../package.json').scripts;
const config = require('../../src-tauri/tauri.conf.json');
const { files, output } = require('../stage-tauri-app.cjs');
const { launch } = require('../run-tauri.cjs');

function checkDefault(name, visited = new Set()) {
  if (visited.has(name)) return;
  visited.add(name);
  const command = scripts[name];
  assert.ok(command, `Missing script ${name}`);
  assert.doesNotMatch(command, /electron|:legacy/i, `${name} must not launch/build Electron`);
  for (const [, dependency] of command.matchAll(/npm run ([\w:-]+)/g)) checkDefault(dependency, visited);
}
for (const name of ['dev', 'build', 'build:ui', 'pack:win', 'dist:win', 'start', 'start:dev', 'start:built', 'start:packaged']) checkDefault(name);
assert.equal(config.build.beforeDevCommand, 'npm run dev:web');
assert.match(scripts['dev:web'], /vite --host 127\.0\.0\.1/);
assert.match(scripts['build:tauri'], /tauri build --no-bundle/);
assert.match(scripts['dist:win'], /tauri bundle --bundles nsis/);
for (const name of ['dev:legacy', 'build:legacy', 'start:legacy', 'pack:legacy', 'dist:legacy']) assert.ok(scripts[name]);
assert.deepEqual(files, ['clipture.exe', 'clipture_engine.exe', 'ffmpeg.exe', 'assets/default.mp3', 'assets/option2.wav']);
assert.equal(output, path.join(root, 'release/tauri-unpacked'));
assert.doesNotMatch(fs.readFileSync(path.join(root, 'scripts/stage-tauri-app.cjs'), 'utf8'), /rmSync|unlinkSync|rmdirSync/);

let calls = 0;
let unreferenced = false;
const fakeSpawn = (executable, args, options) => {
  calls++;
  assert.equal(executable, path.join(output, 'clipture.exe'));
  assert.deepEqual(args, ['--hidden']);
  assert.equal(options.cwd, output);
  assert.equal(options.env, process.env, 'Isolated-profile overrides must be inherited');
  assert.equal(options.detached, true);
  assert.equal(options.windowsHide, true);
  assert.equal(options.stdio, 'ignore');
  const child = new EventEmitter();
  child.unref = () => { unreferenced = true; };
  return child;
};
launch(['--hidden'], fakeSpawn, () => true);
assert.equal(calls, 1);
assert.ok(unreferenced, 'Do not keep a Node launcher alive at tray idle');
assert.throws(() => launch([], fakeSpawn, () => false), /run npm run build first/);
assert.equal(calls, 1, 'Missing runtime files must fail before spawning');
console.log('Tauri workflow passed: default scripts, Vite startup, five-file payload, detached launch, legacy reference.');
