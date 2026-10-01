const fs = require("node:fs");
const path = require("node:path");
const { createHash } = require("node:crypto");
const { execFileSync } = require("node:child_process");
const { treeHash } = require("./create-runtime-manifest.cjs");

function nativeIdentity(root, toolchain) {
  const hash = createHash("sha256").update(toolchain);
  for (const directory of ["src-tauri/src", "src-tauri/capabilities"]) {
    hash.update(directory).update(treeHash(path.join(root, directory)));
  }
  for (const directory of [".cargo", "src-tauri/.cargo"]) {
    hash.update(directory).update(fs.existsSync(path.join(root, directory)) ? treeHash(path.join(root, directory)) : "absent");
  }
  for (const name of ["src-tauri/build.rs", "src-tauri/Cargo.toml", "src-tauri/Cargo.lock", "src-tauri/tauri.conf.json", "src-tauri/tauri.release.conf.json"]) {
    hash.update(name).update(identityContent(name, fs.readFileSync(path.join(root, name), "utf8")));
  }
  return hash.digest("hex");
}

function identityContent(name, content) {
  const version = /^version\s*=\s*"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?"\s*$/m;
  if (name.endsWith("Cargo.toml")) {
    return content.replace(/(\[package\]\s*\r?\n)([\s\S]*?)(?=^\[|$(?![\s\S]))/m,
      (_, heading, body) => heading + body.replace(version, 'version = "0.0.0"'));
  }
  if (name.endsWith("Cargo.lock")) {
    return content.replace(/\[\[package\]\]\r?\nname = "clipture"\r?\nversion = "[^"\r\n]+"/g,
      '[[package]]\nname = "clipture"\nversion = "0.0.0"');
  }
  if (name.endsWith(".json")) {
    const value = JSON.parse(content);
    if (typeof value.version === "string" && /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/.test(value.version)) value.version = "0.0.0";
    return JSON.stringify(value);
  }
  return content;
}

if (require.main === module) {
  const root = path.resolve(__dirname, "../..");
  console.log(nativeIdentity(root, execFileSync("rustc", ["-vV"], { encoding: "utf8" })));
}
module.exports = { nativeIdentity, identityContent };
