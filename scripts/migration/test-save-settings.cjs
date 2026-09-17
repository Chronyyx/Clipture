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
const fpsExports = {};
new Function('exports', ts.transpileModule(read('src/shared/capture-fps.ts'),
  { compilerOptions: { module: ts.ModuleKind.CommonJS } }).outputText)(fpsExports);
new Function('exports', 'require', compiled)(moduleExports, name => {
  assert.equal(name, '../../shared/capture-fps');
  return fpsExports;
});
const normalize = moduleExports.normalizeSaveSettings;
assert.equal(normalize({}).saveInPlace, fixture.settingsDefaults.saveInPlace);
assert.equal(normalize({ saveInPlace: false }).saveInPlace, false);
assert.deepEqual(normalize({ saveInPlace: true, hotkey: 'fixture' }), { saveInPlace: true, hotkey: 'fixture', fps: 30 });
assert.deepEqual(fpsExports.CAPTURE_FPS_OPTIONS, fixture.captureFpsOptions);
for (const fps of fixture.captureFpsOptions) {
  assert.equal(normalize({ fps }).fps, fps);
  assert.equal(normalize(JSON.parse(JSON.stringify(normalize({ fps })))).fps, fps);
}
for (const fps of [undefined, null, 0, 59, 61, 119, 241, 999, NaN, Infinity]) {
  assert.equal(normalize({ fps }).fps, 30);
}
assert.match(read('src/main/main.ts'), /const fps = normalizeCaptureFps\(settings.fps\)/);
assert.match(read('src/renderer/features/settings/VideoSettings.tsx'), /CAPTURE_FPS_OPTIONS.map/);
for (const adapter of ['electron-adapter.ts', 'tauri-adapter.ts']) {
  const text = read('src/renderer/platform/' + adapter);
  assert.match(text, /getSettings:.*normalizeSaveSettings/);
  assert.match(text, /saveSettings:.*normalizeSaveSettings/);
}
assert.match(read('engine/src/main.cpp'), /extractBool\(line, "saveInPlace", true\)/);
assert.match(read('src-tauri/src/contracts/engine.rs'), /save_in_place: settings.save_in_place/);
console.log('Save settings: default-on, explicit opt-out and both host adapters passed.');
