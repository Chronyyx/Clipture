'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const root = path.resolve(__dirname, '../..');

function load(relative, stubs, cache = new Map()) {
  const file = path.resolve(root, relative);
  if (cache.has(file)) return cache.get(file);
  const exports = {};
  cache.set(file, exports);
  const compiled = ts.transpileModule(fs.readFileSync(file, 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 }
  }).outputText;
  new Function('exports', 'require', compiled)(exports, name => {
    if (name in stubs) return stubs[name];
    assert.ok(name.startsWith('.'), 'Unexpected dependency: ' + name);
    return load(path.resolve(path.dirname(file), name + '.ts'), stubs, cache);
  });
  return exports;
}

async function run() {
  const fixture = JSON.parse(fs.readFileSync(path.join(root,
    'scripts/migration/fixtures/host-contract.v1.json'), 'utf8'));
  let stored;
  const save = async settings => {
    stored = JSON.parse(JSON.stringify(settings));
    return stored;
  };
  const stubs = {
    '@tauri-apps/api/core': {
      invoke: async (command, args) => {
        if (command === 'get_settings') return stored;
        assert.equal(command, 'save_settings');
        return save(args.settings);
      },
      convertFileSrc: value => value
    },
    '@tauri-apps/api/event': { listen: async () => () => {} }
  };
  const electron = load('src/renderer/platform/electron-adapter.ts', stubs)
    .createElectronAdapter({ getSettings: async () => stored, saveSettings: save });
  const tauri = load('src/renderer/platform/tauri-adapter.ts', stubs).createTauriAdapter();
  for (const adapter of [electron, tauri]) {
    for (const fps of fixture.captureFpsOptions) {
      const input = { fps, saveInPlace: false, saveInPlaceOverlap: false, clipLengthSeconds: 120 };
      assert.deepEqual(await adapter.saveSettings(input), input);
      assert.equal(stored.fps, fps, 'Host receives the selected target unchanged');
      assert.deepEqual(await adapter.getSettings(), input);
    }
    for (const fps of [144, 210, 240, 999]) {
      stored = { fps, saveInPlace: false };
      assert.equal((await adapter.getSettings()).fps, 30);
      assert.equal((await adapter.saveSettings({ fps, saveInPlace: false })).fps, 30);
      assert.equal(stored.fps, 30);
      assert.equal(stored.saveInPlace, false);
    }
  }
  console.log('FPS adapters: every choice survives both real adapter save/get paths.');
}
module.exports = run();
module.exports.catch(error => { console.error(error); process.exitCode = 1; });
