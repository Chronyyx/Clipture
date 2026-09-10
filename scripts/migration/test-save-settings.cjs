'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const root = path.resolve(__dirname, '../..');
const read = name => fs.readFileSync(path.join(root, name), 'utf8');
const fixture = JSON.parse(read('scripts/migration/fixtures/host-contract.v1.json'));
const compiled = ts.transpileModule(read('src/renderer/platform/save-settings.ts'),
  { compilerOptions: { module: ts.ModuleKind.CommonJS } }).outputText;
const moduleExports = {};
new Function('exports', compiled)(moduleExports);
const normalize = moduleExports.normalizeSaveSettings;
assert.equal(normalize({}).saveInPlace, fixture.settingsDefaults.saveInPlace);
assert.equal(normalize({ saveInPlace: false }).saveInPlace, false);
assert.deepEqual(normalize({ saveInPlace: true, hotkey: 'fixture' }), { saveInPlace: true, hotkey: 'fixture' });
for (const adapter of ['electron-adapter.ts', 'tauri-adapter.ts']) {
  const text = read('src/renderer/platform/' + adapter);
  assert.match(text, /getSettings:.*normalizeSaveSettings/);
  assert.match(text, /saveSettings:.*normalizeSaveSettings/);
}
assert.match(read('engine/src/main.cpp'), /extractBool\(line, "saveInPlace", true\)/);
assert.match(read('src-tauri/src/contracts/engine.rs'), /save_in_place: settings.save_in_place/);
console.log('Save settings: default-on, explicit opt-out and both host adapters passed.');
