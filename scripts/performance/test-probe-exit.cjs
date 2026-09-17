'use strict';
// Reproduce an engine exiting after configure but before the next request.
// No capture, real child process, or user profile is involved.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { EventEmitter } = require('node:events');
const { PassThrough } = require('node:stream');
const childProcess = require('node:child_process');
const originalSpawn = childProcess.spawn;
childProcess.spawn = () => {
  const child = new EventEmitter();
  child.stdin = new PassThrough(); child.stdout = new PassThrough(); child.stderr = new PassThrough();
  child.exitCode = child.signalCode = null;
  child.kill = () => { throw Error('An already-exited fixture must not need a watchdog'); };
  child.stdin.once('data', data => {
    const request = JSON.parse(data.toString());
    child.stdout.write(JSON.stringify({ id: request.id, payload: {} }) + '\n');
    setTimeout(() => {
      child.stderr.write('fixture: exited between requests\n');
      child.exitCode = 23;
      child.emit('exit', 23);
      child.stdout.end(); child.stderr.end();
    }, 10);
  });
  return child;
};
const { probe } = require('./probe-engine-cadence.cjs');
childProcess.spawn = originalSpawn;
async function main() {
  const directory = fs.mkdtempSync(path.resolve('.cache/cs2-perf/probe-exit-test-'));
  await assert.rejects(probe('fixture.exe', 120, path.join(directory, 'trial'), 1), /Engine exited \(23\)/);
  assert.match(fs.readFileSync(path.join(directory, 'trial', 'engine.log'), 'utf8'), /exited between requests/);
  console.log('Probe rejects exits between requests and preserves failure logs.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
