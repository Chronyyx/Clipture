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
// The engine's clip content verdict: every field it sends is declared by both hosts.
const contentFields = Object.keys(JSON.parse(fs.readFileSync(path.join(root, 'scripts/migration/fixtures/engine-protocol.v1.json'), 'utf8')).examples.segmentedSave.payload.contentCheck);
const typesText = fs.readFileSync(path.join(root, 'src/shared/types.ts'), 'utf8');
const tsCheck = typesText.slice(typesText.indexOf('export interface ClipContentCheck'));
const tsFields = [...tsCheck.slice(0, tsCheck.indexOf('\n}')).matchAll(/^\s{2}([a-zA-Z]+):/gm)].map((match) => match[1]);
assert.deepEqual([...tsFields].sort(), [...contentFields].sort(), 'ClipContentCheck in types.ts differs from the engine fixture');
const rustText = fs.readFileSync(path.join(root, 'src-tauri/src/contracts/engine.rs'), 'utf8');
const rustCheck = rustText.slice(rustText.indexOf('pub struct ClipContentCheck'));
const rustFields = [...rustCheck.slice(0, rustCheck.indexOf('\n}')).matchAll(/^\s{4}pub ([a-z_]+):/gm)]
  .map((match) => match[1].replace(/_([a-z])/g, (_, c) => c.toUpperCase()));
assert.deepEqual([...rustFields].sort(), [...contentFields].sort(), 'ClipContentCheck in engine.rs differs from the engine fixture');
const engineText = fs.readFileSync(path.join(root, 'engine/src/ClipContentCheck.cpp'), 'utf8');
for (const field of contentFields) assert.ok(engineText.includes(`\\"${field}\\"`), `${field}: the engine does not write it`);

console.log('Save-result contract passed: sparse segment audio metadata and the clip content check survive both host adapters.');
