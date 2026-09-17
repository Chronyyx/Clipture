'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const root = path.resolve(__dirname, '../..');
const load = (file, dependencies = {}) => {
  const exports = {};
  const source = ts.transpileModule(fs.readFileSync(path.join(root, file), 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 }
  }).outputText;
  new Function('exports', 'require', source)(exports, name => {
    assert.ok(name in dependencies, 'Unexpected dependency: ' + name);
    return dependencies[name];
  });
  return exports;
};

async function run() {
  const platform = load('src/renderer/platform/hostError.ts');
  const { preparePlayback } = load('src/renderer/features/player/preparePlayback.ts', { '../../platform': platform });
  const busy = new platform.HostCapabilityError('tauri', 'clipPlaybackUrl', 'UI host is busy; retry shortly');
  assert.equal(platform.isHostBusyError(busy), true);
  assert.doesNotMatch(busy.message, /registered|implemented/);
  assert.equal(platform.isHostBusyError(new Error('UI host is busy; retry shortly')), false);
  const timers = new Map();
  const originalSet = global.setTimeout, originalClear = global.clearTimeout;
  let timerId = 0;
  global.setTimeout = (callback, ms) => { timers.set(++timerId, { callback, ms }); return timerId; };
  global.clearTimeout = id => timers.delete(id);
  const flush = async () => { for (let i = 0; i < 5; i++) await Promise.resolve(); };
  const tick = async expected => {
    const [id, timer] = timers.entries().next().value;
    assert.equal(timer.ms, expected); timers.delete(id); timer.callback(); await flush();
  };
  try {
    let calls = 0, ready = 0, failures = 0;
    preparePlayback(async () => { if (++calls < 3) throw busy; return 'ok'; },
      value => { assert.equal(value, 'ok'); ready++; }, () => failures++);
    await flush(); await tick(100); await tick(250);
    assert.deepEqual([calls, ready, failures, timers.size], [3, 1, 0, 0]);

    calls = 0;
    preparePlayback(async () => { calls++; throw busy; }, () => assert.fail('unexpected success'), () => failures++);
    await flush(); await tick(100); await tick(250); await tick(500);
    assert.deepEqual([calls, failures, timers.size], [4, 1, 0]);

    calls = 0;
    const cancel = preparePlayback(async () => { calls++; throw busy; }, () => ready++, () => failures++);
    await flush(); cancel();
    assert.equal(timers.size, 0); assert.equal(calls, 1);

    let resolve;
    const cancelInFlight = preparePlayback(() => new Promise(done => { resolve = done; }), () => ready++, () => failures++);
    cancelInFlight(); resolve('stale'); await flush();
    assert.equal(ready, 1, 'Old clip result must not replace new playback');

    const timeout = new platform.HostCapabilityError('tauri', 'clipPlaybackUrl', 'timed out');
    preparePlayback(async () => { throw timeout; }, () => ready++, error => { assert.equal(error, timeout); failures++; });
    await flush(); assert.equal(timers.size, 0); assert.equal(failures, 2);
    console.log('Playback busy: bounded retries, cancellation, late-result suppression and no timeout retries passed.');
  } finally {
    global.setTimeout = originalSet; global.clearTimeout = originalClear;
  }
}
module.exports = run();
module.exports.catch(error => { console.error(error); process.exitCode = 1; });
