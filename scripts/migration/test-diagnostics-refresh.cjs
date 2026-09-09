'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const root = path.resolve(__dirname, '../..');

// Exercise the real adapters/reducer without launching a host or capture.
function load(relative, stubs = {}, cache = new Map()) {
  const file = path.resolve(root, relative);
  if (cache.has(file)) return cache.get(file);
  const exports = {};
  cache.set(file, exports);
  const source = fs.readFileSync(file, 'utf8');
  const compiled = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 }
  }).outputText;
  new Function('exports', 'require', compiled)(exports, name => {
    if (name in stubs) return stubs[name];
    assert.ok(name.startsWith('.'), 'Unexpected runtime dependency: ' + name);
    return load(path.resolve(path.dirname(file), name + '.ts'), stubs, cache);
  });
  return exports;
}

async function run() {
  const fixture = JSON.parse(fs.readFileSync(path.join(root, 'scripts/migration/fixtures/host-contract.v1.json'), 'utf8'));
  assert.equal(fixture.operations.find(op => op.method === 'getDiagnostics').adapterFailure,
    'reject-without-fabricating-engine-state');
  const { diagnosticsSnapshot: reduce, initialDiagnosticsSnapshot: initial } = load('src/renderer/features/diagnostics/diagnosticsSnapshot.ts');
  let reply;
  let failure;
  const read = async () => { if (failure) throw failure; return reply; };
  const electron = load('src/renderer/platform/electron-adapter.ts').createElectronAdapter({ getDiagnostics: read });
  const tauri = load('src/renderer/platform/tauri-adapter.ts', {
    '@tauri-apps/api/core': { invoke: read, convertFileSrc: value => value },
    '@tauri-apps/api/event': { listen: async () => () => {} }
  }).createTauriAdapter();

  for (const [name, adapter] of [['electron', electron], ['tauri', tauri]]) {
    failure = undefined;
    reply = { activeEncoder: 'NVENC', encoderMode: 'Hardware', gpu: 'Fixture GPU', engineRunning: true, degraded: false };
    let state = reduce(initial, { type: 'received', diagnostics: await adapter.getDiagnostics() });
    const healthy = state.diagnostics;
    failure = new Error('getDiagnostics timed out while saving');
    for (let attempt = 0; attempt < 6; attempt++) {
      await assert.rejects(adapter.getDiagnostics(), /timed out/);
      try { await adapter.getDiagnostics(); }
      catch (error) { state = reduce(state, { type: 'delayed', error }); }
      assert.equal(state.diagnostics, healthy, name + ': retain the exact last snapshot');
      assert.match(state.error, /timed out/);
      assert.equal(state.received, true);
    }
    const waiting = reduce(initial, { type: 'delayed', error: failure });
    assert.equal(waiting.received, false, 'Opening during a save must stay waiting, not claim a report');
    failure = undefined;
    reply = { ...reply, capturedFrames: 800 };
    state = reduce(state, { type: 'received', diagnostics: await adapter.getDiagnostics() });
    assert.equal(state.error, undefined, 'Recovery clears stale status');
    assert.equal(state.diagnostics.capturedFrames, 800);
    reply = { activeEncoder: 'Unavailable', gpu: 'Fixture GPU', engineRunning: false, degraded: true, status: 'Engine exited' };
    state = reduce(state, { type: 'received', diagnostics: await adapter.getDiagnostics() });
    assert.equal(state.diagnostics.engineRunning, false, 'Do not conceal a real offline report');
    assert.equal(state.diagnostics.activeEncoder, 'Unavailable');
    assert.equal(state.error, undefined);
  }
  console.log('Diagnostics refresh passed: both adapters, repeated timeout, initial wait, recovery, and real offline report.');
}

module.exports = run();
module.exports.catch(error => { console.error(error); process.exitCode = 1; });
