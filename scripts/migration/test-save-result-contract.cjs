'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const root = path.resolve(__dirname, '../..');
const source = fs.readFileSync(path.join(root, 'src/renderer/platform/save-result.ts'), 'utf8');
const compiled = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.CommonJS } }).outputText;
const exportsForTest = {};
new Function('exports', compiled)(exportsForTest);
const fixture = JSON.parse(fs.readFileSync(path.join(root, 'scripts/migration/fixtures/engine-protocol.v1.json'), 'utf8')).examples.segmentedSave.payload;
const preserved = exportsForTest.preserveSaveResult(fixture);
assert.deepEqual(preserved, fixture);
assert.notEqual(preserved.clip.segmentAudioTracks, fixture.clip.segmentAudioTracks);
assert.throws(() => exportsForTest.preserveSaveResult({ ...fixture, clip: { ...fixture.clip, segmentAudioTracks: [[]] } }), /inconsistent/);
for (const adapter of ['electron-adapter.ts', 'tauri-adapter.ts']) {
  const text = fs.readFileSync(path.join(root, 'src/renderer/platform', adapter), 'utf8');
  assert.match(text, /saveClip:.*preserveSaveResult/s, adapter + ' must preserve the additive save metadata');
}
console.log('Save-result contract passed: sparse segment audio metadata survives both host adapters.');
