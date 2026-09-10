"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const repositoryRoot = path.resolve(__dirname, "..", "..");
const read = (relativePath) => fs.readFileSync(path.join(repositoryRoot, relativePath), "utf8");
const readJson = (relativePath) => JSON.parse(read(relativePath));
const sorted = (values) => [...values].sort((left, right) => left.localeCompare(right));

function assertUnique(values, label) {
  assert.equal(new Set(values).size, values.length, `${label} contains duplicates`);
}

function cliptureApiMethods(source) {
  const lines = source.split(/\r?\n/);
  const start = lines.findIndex((line) => /^export interface CliptureApi\s*\{/.test(line));
  assert.notEqual(start, -1, "CliptureApi interface was not found");
  const end = lines.findIndex((line, index) => index > start && /^\}/.test(line));
  assert.notEqual(end, -1, "CliptureApi interface is not closed");
  return lines.slice(start + 1, end).flatMap((line) => {
    const match = line.match(/^\s*([A-Za-z][A-Za-z0-9_]*)\s*(?:\(|:)/);
    return match ? [match[1]] : [];
  });
}

function preloadOperations(source) {
  const lines = source.split(/\r?\n/);
  const start = lines.findIndex((line) => /^const api:\s*CliptureApi\s*=\s*\{/.test(line));
  assert.notEqual(start, -1, "preload CliptureApi object was not found");
  const entries = [];
  for (let index = start + 1; index < lines.length; index += 1) {
    if (/^\};/.test(lines[index])) break;
    const match = lines[index].match(/^  ([A-Za-z][A-Za-z0-9_]*):/);
    if (match) entries.push({ method: match[1], line: index });
  }
  return entries.map((entry, index) => ({
    method: entry.method,
    source: lines.slice(entry.line, entries[index + 1]?.line ?? lines.length).join("\n")
  }));
}

const hostFixture = readJson("scripts/migration/fixtures/host-contract.v1.json");
const engineFixture = readJson("scripts/migration/fixtures/engine-protocol.v1.json");
const typeSource = read("src/shared/types.ts");
const preloadSource = read("src/preload/preload.ts");
const mainSource = read("src/main/main.ts");
const engineSource = read("engine/src/main.cpp");

assert.equal(hostFixture.schemaVersion, 1, "unsupported host fixture version");
assert.ok(Array.isArray(hostFixture.operations) && hostFixture.operations.length > 0);
const fixtureMethods = hostFixture.operations.map((operation) => operation.method);
const interfaceMethods = cliptureApiMethods(typeSource);
const preloadEntries = preloadOperations(preloadSource);
assertUnique(fixtureMethods, "host fixture");
assertUnique(interfaceMethods, "CliptureApi");
assertUnique(preloadEntries.map((entry) => entry.method), "preload api");
assert.deepEqual(sorted(fixtureMethods), sorted(interfaceMethods), "fixture and CliptureApi methods differ");
assert.deepEqual(sorted(fixtureMethods), sorted(preloadEntries.map((entry) => entry.method)), "fixture and preload methods differ");

for (const operation of hostFixture.operations) {
  assert.ok(["invoke", "send", "event"].includes(operation.transport), `${operation.method}: invalid transport`);
  assert.match(operation.channel, /^[a-z][A-Za-z]*(?::[A-Za-z][A-Za-z]*)?$|^[a-z]+-[a-z-]+$/, `${operation.method}: invalid channel`);
  const entry = preloadEntries.find((candidate) => candidate.method === operation.method);
  assert.ok(entry.source.includes(JSON.stringify(operation.channel)), `${operation.method}: channel mapping differs`);
  const requiredCall = operation.transport === "invoke" ? "ipcRenderer.invoke(" : operation.transport === "send" ? "ipcRenderer.send(" : "ipcRenderer.on(";
  assert.ok(entry.source.includes(requiredCall), `${operation.method}: expected ${operation.transport} transport`);
  if (operation.transport === "event") {
    assert.ok(entry.source.includes("removeListener("), `${operation.method}: event has no unsubscribe path`);
  }
}

assert.equal(engineFixture.schemaVersion, 1, "unsupported engine fixture version");
assert.equal(engineFixture.framing, "utf8-json-lines");
const commandNames = engineFixture.commands.map((command) => command.type);
assertUnique(commandNames, "engine commands");
for (const command of engineFixture.commands) {
  assert.ok(mainSource.includes(JSON.stringify(command.type)), `${command.type}: missing from Electron engine client`);
  assert.ok(engineSource.includes(command.type), `${command.type}: missing from C++ engine`);
  assertUnique(command.fields, `${command.type} fields`);
  for (const field of command.fields) {
    assert.ok(mainSource.includes(field), `${command.type}.${field}: missing from Electron request`);
    assert.ok(engineSource.includes(field), `${command.type}.${field}: missing from C++ parser`);
  }
}
for (const event of engineFixture.events) {
  assert.ok(mainSource.includes(JSON.stringify(event.event)), `${event.event}: missing from Electron event client`);
  assert.ok(engineSource.includes(event.event), `${event.event}: missing from C++ engine`);
}

const examples = engineFixture.examples;
require('./test-save-result-contract.cjs');
require('./test-save-settings.cjs');
require('./test-diagnostics-refresh.cjs');
require('./test-ui-process-boundaries.cjs');
assert.deepEqual(Object.keys(examples.request).sort(), ["id", "type"]);
assert.ok("id" in examples.success && "payload" in examples.success);
assert.ok("id" in examples.error && "error" in examples.error);
assert.ok("event" in examples.event);

console.log(`Host contract OK: ${fixtureMethods.length} browser operations, ${commandNames.length} engine commands, ${engineFixture.events.length} engine event.`);
