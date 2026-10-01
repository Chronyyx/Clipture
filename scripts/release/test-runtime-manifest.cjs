const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const crypto = require("node:crypto");
const { FILES, BLOCK_SIZE, sha256, createManifest, treeHash } = require("./create-runtime-manifest.cjs");
const { verifyManifest } = require("./verify-runtime-manifest.cjs");

const { identityContent } = require("./native-identity.cjs");
for (const [name, content] of [
  ["src-tauri/Cargo.toml", '[package]\nname = "clipture"\nversion = "1.5.4"\n[dependencies]\nthing = "1.5.4"\n'],
  ["src-tauri/Cargo.lock", '[[package]]\nname = "clipture"\nversion = "1.5.4"\n[[package]]\nname = "dependency"\nversion = "1.5.4"\n'],
  ["src-tauri/tauri.conf.json", '{"version":"1.5.4","identifier":"app.clipture.desktop"}']
]) {
  const baseline = identityContent(name, content);
  assert.equal(baseline, identityContent(name, content.replace("1.5.4", "1.5.5")));
  assert.notEqual(baseline, identityContent(name, content.replace("clipture", "changed")));
  if (!name.endsWith(".json")) assert.notEqual(baseline, identityContent(name, content.replaceAll("1.5.4", "1.5.5")));
}

const root = fs.mkdtempSync(path.join(os.tmpdir(), "clipture-runtime-publisher-"));
try {
  fs.mkdirSync(path.join(root, "assets"));
  for (const file of FILES) fs.writeFileSync(path.join(root, file), "test");
  fs.writeFileSync(path.join(root, FILES[0]), Buffer.alloc(BLOCK_SIZE + 5, 7));
  const manifest = createManifest(root, "9.9.9", "a".repeat(64), treeHash(root));
  assert.equal(manifest.files.length, 5);
  assert.equal(manifest.files[0].blocks.length, 2);
  assert.equal(manifest.files[0].blocks[1], sha256(Buffer.alloc(5, 7)));
  assert.equal(manifest.components.engine, manifest.files[1].sha256);
  assert.deepEqual(manifest, createManifest(root, "9.9.9", "a".repeat(64), treeHash(root)));
  assert.throws(() => createManifest(root, "../9.9.9", "a".repeat(64), "b".repeat(64)));
  assert.throws(() => createManifest(root, "9.9.9", "bad", "b".repeat(64)));
  const bytes = Buffer.from(JSON.stringify(manifest));
  const { publicKey, privateKey } = crypto.generateKeyPairSync("ed25519");
  const rawKey = publicKey.export({ format: "der", type: "spki" }).subarray(-32);
  const id = Buffer.alloc(8, 3);
  const encode = (value) => Buffer.from(value).toString("base64");
  const key = encode(`untrusted comment: test only\n${encode(Buffer.concat([Buffer.from("Ed"), id, rawKey]))}\n`);
  const signature = crypto.sign(null, crypto.createHash("blake2b512").update(bytes).digest(), privateKey);
  const trusted = "test fixture";
  const global = crypto.sign(null, Buffer.concat([signature, Buffer.from(trusted)]), privateKey);
  const envelope = encode(`untrusted comment: test only\n${encode(Buffer.concat([Buffer.from("ED"), id, signature]))}\ntrusted comment: ${trusted}\n${encode(global)}\n`);
  verifyManifest(bytes, key, envelope);
  assert.throws(() => verifyManifest(Buffer.concat([bytes, Buffer.from(" ")]), key, envelope));
  assert.throws(() => verifyManifest(bytes, key, "invalid"));
  fs.unlinkSync(path.join(root, FILES[1]));
  assert.throws(() => createManifest(root, "9.9.9", "a".repeat(64), "b".repeat(64)));
  console.log("Runtime publisher: bounded files, blocks, identities, signatures and tampering passed.");
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
