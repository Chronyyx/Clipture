'use strict';
// Static, read-only check of the friend-sharing seam (ADR 0011): the typed
// SharingApi, the Tauri adapter, the unsupported/mock adapters, the Rust
// command registry, the disposable-UI allowlist and the event relay agree.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '../..');
const read = (file) => fs.readFileSync(path.join(root, file), 'utf8');
const fixture = JSON.parse(read('scripts/migration/fixtures/sharing-contract.v1.json'));
const sorted = (values) => [...values].sort();

assert.equal(fixture.schemaVersion, 1, 'unsupported sharing fixture version');
const types = read('src/shared/sharing.ts');
const body = types.slice(types.indexOf('export interface SharingApi'));
const interfaceMethods = [...body.slice(0, body.indexOf('\n}')).matchAll(/^\s{2}([a-zA-Z]+)\(/gm)].map((match) => match[1]);
const fixtureMethods = [...fixture.operations.map((operation) => operation.method), fixture.eventMethod];
assert.deepEqual(sorted(interfaceMethods), sorted(fixtureMethods), 'fixture and SharingApi methods differ');

const tauri = read('src/renderer/platform/tauri-sharing-adapter.ts');
const lib = read('src-tauri/src/lib.rs');
const dispatch = read('src-tauri/src/app/ui_process/dispatch.rs');
for (const operation of fixture.operations) {
  assert.ok(tauri.includes(`${operation.method}: '${operation.command}'`), `${operation.method}: Tauri command mapping differs`);
  assert.ok(lib.includes(`commands::${operation.command},`), `${operation.command}: not registered in lib.rs`);
  const arm = dispatch.slice(dispatch.indexOf(`"${operation.command}" =>`));
  assert.ok(arm.length < dispatch.length, `${operation.command}: missing from the disposable UI dispatcher`);
  const armBody = arm.slice(0, arm.indexOf('\n        "', 1));
  for (const argument of operation.args) {
    assert.ok(armBody.includes(`"${argument}"`), `${operation.command}.${argument}: dispatcher does not read it`);
    assert.ok(tauri.includes(argument), `${operation.command}.${argument}: adapter does not send it`);
  }
  assert.doesNotMatch(armBody, /"owner"/, `${operation.command}: ownership must come from the connection`);
}

assert.ok(tauri.includes(`'${fixture.event}'`), 'Tauri adapter does not subscribe to the sharing event');
assert.ok(read('src-tauri/src/app/ui_process/output.rs').includes(`"${fixture.event}"`), 'sharing event is not relayed to the UI worker');
assert.ok(read('src-tauri/src/app/sharing_events.rs').includes(`"${fixture.event}"`), 'controller does not emit the sharing event');
assert.ok(read('src-tauri/src/sharing/wire.rs').includes(`b"${fixture.peerAlpn}"`), 'peer ALPN differs from the fixture');

const unsupported = read('src/renderer/platform/sharing-client.ts');
const mock = read('src/renderer/platform/mock-sharing-adapter.ts');
for (const method of fixtureMethods) {
  assert.ok(unsupported.includes(` ${method}:`), `${method}: missing from the unsupported adapter`);
  assert.ok(mock.includes(` ${method}:`), `${method}: missing from the mock adapter`);
}

// Features reach the host only through the platform client.
const featureDir = path.join(root, 'src/renderer/features/sharing');
for (const file of fs.readdirSync(featureDir)) {
  const source = fs.readFileSync(path.join(featureDir, file), 'utf8');
  assert.doesNotMatch(source, /@tauri-apps|electron|window\.clipture/, `${file}: sharing feature touches a host directly`);
}

console.log(`Sharing contract passed: ${fixture.operations.length} commands, 1 event, peer ALPN ${fixture.peerAlpn}.`);
