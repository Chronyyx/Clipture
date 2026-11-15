'use strict';
// Static, read-only check of the telemetry seam (ADR 0035): the typed
// TelemetryApi, the Tauri adapter, the unsupported/mock adapters, the Rust
// command registry, the disposable-UI dispatcher and the status shape agree.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '../..');
const read = (file) => fs.readFileSync(path.join(root, file), 'utf8');
const fixture = JSON.parse(read('scripts/migration/fixtures/telemetry-contract.v1.json'));
const sorted = (values) => [...values].sort();

assert.equal(fixture.schemaVersion, 1, 'unsupported telemetry fixture version');
const types = read('src/shared/telemetry.ts');
const api = types.slice(types.indexOf('export interface TelemetryApi'));
const methods = [...api.slice(0, api.indexOf('\n}')).matchAll(/^\s{2}([a-zA-Z]+)\(/gm)].map((match) => match[1]);
assert.deepEqual(sorted(methods), sorted(fixture.operations.map((operation) => operation.method)), 'fixture and TelemetryApi methods differ');

const status = types.slice(types.indexOf('export interface TelemetryStatus'));
const statusFields = [...status.slice(0, status.indexOf('\n}')).matchAll(/^\s{2}([a-zA-Z]+):/gm)].map((match) => match[1]);
assert.deepEqual(sorted(statusFields), sorted(fixture.statusFields), 'TelemetryStatus fields differ from the fixture');
const rust = read('src-tauri/src/telemetry/mod.rs');
const rustStatus = rust.slice(rust.indexOf('pub struct TelemetryStatus'));
const rustFields = [...rustStatus.slice(0, rustStatus.indexOf('\n}')).matchAll(/^\s{4}pub ([a-z_]+):/gm)]
  .map((match) => match[1].replace(/_([a-z])/g, (_, c) => c.toUpperCase()));
assert.deepEqual(sorted(rustFields), sorted(fixture.statusFields), 'Rust TelemetryStatus fields differ from the fixture');

const tauri = read('src/renderer/platform/tauri-telemetry-adapter.ts');
const lib = read('src-tauri/src/lib.rs');
const dispatch = read('src-tauri/src/app/ui_process/dispatch.rs');
for (const operation of fixture.operations) {
  assert.ok(tauri.includes(`${operation.method}: '${operation.command}'`), `${operation.method}: Tauri command mapping differs`);
  assert.ok(lib.includes(`telemetry::commands::${operation.command},`), `${operation.command}: not registered in lib.rs`);
  const arm = dispatch.slice(dispatch.indexOf(`"${operation.command}" =>`));
  assert.ok(arm.length < dispatch.length, `${operation.command}: missing from the disposable UI dispatcher`);
  const armBody = arm.slice(0, arm.indexOf('\n        "', 1));
  for (const argument of operation.args) {
    assert.ok(armBody.includes(`"${argument}"`), `${operation.command}.${argument}: dispatcher does not read it`);
    assert.ok(tauri.includes(`{ ${argument} }`), `${operation.command}.${argument}: adapter does not send it`);
  }
}

const client = read('src/renderer/platform/telemetry-client.ts');
const unsupported = client.slice(client.indexOf('function createUnsupportedTelemetryAdapter'), client.indexOf('export function createMockTelemetryAdapter'));
const mock = client.slice(client.indexOf('export function createMockTelemetryAdapter'));
for (const { method } of fixture.operations) {
  assert.ok(unsupported.includes(` ${method}:`), `${method}: missing from the unsupported adapter`);
  assert.ok(mock.includes(` ${method}:`), `${method}: missing from the mock adapter`);
}

// The feature reaches the host only through the platform client.
const featureDir = path.join(root, 'src/renderer/features/telemetry');
for (const file of fs.readdirSync(featureDir)) {
  const source = fs.readFileSync(path.join(featureDir, file), 'utf8');
  assert.doesNotMatch(source, /@tauri-apps|electron|window\.clipture/, `${file}: telemetry feature touches a host directly`);
}

console.log(`Telemetry contract passed: ${fixture.operations.length} commands, ${fixture.statusFields.length} status fields.`);
