const fs = require("node:fs");
const path = require("node:path");
const { createHash } = require("node:crypto");

const FILES = ["clipture.exe", "clipture_engine.exe", "ffmpeg.exe", "assets/default.mp3", "assets/option2.wav"];
const BLOCK_SIZE = 1048576;
const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

function describeFile(root, relative, version) {
  const filename = path.join(root, relative);
  const stat = fs.lstatSync(filename);
  if (!stat.isFile() || stat.isSymbolicLink() || stat.size === 0 || stat.size > 512 * 1024 * 1024) {
    throw new Error(`Invalid runtime file: ${relative}`);
  }
  const fd = fs.openSync(filename, "r");
  const hash = createHash("sha256");
  const blocks = [];
  const buffer = Buffer.alloc(BLOCK_SIZE);
  try {
    for (let offset = 0; offset < stat.size; offset += BLOCK_SIZE) {
      const length = Math.min(BLOCK_SIZE, stat.size - offset);
      let read = 0;
      while (read < length) {
        const count = fs.readSync(fd, buffer, read, length - read, offset + read);
        if (!count) throw new Error(`Truncated runtime file: ${relative}`);
        read += count;
      }
      const block = buffer.subarray(0, length);
      blocks.push(sha256(block));
      hash.update(block);
    }
  } finally {
    fs.closeSync(fd);
  }
  return {
    path: relative, size: stat.size, sha256: hash.digest("hex"),
    url: `https://github.com/Chronyyx/Clipture/releases/download/v${version}/${path.basename(relative)}`,
    blocks, blockSize: BLOCK_SIZE,
  };
}

function createManifest(root, version, nativeId, uiId) {
  if (!/^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.test(version)) throw new Error("Expected stable canonical version");
  if (![nativeId, uiId].every((value) => /^[a-f0-9]{64}$/.test(value))) throw new Error("Missing component identities");
  const files = FILES.map((file) => describeFile(root, file, version));
  if (files.reduce((total, file) => total + file.size, 0) > 1024 * 1024 * 1024) throw new Error("Runtime exceeds size limit");
  return {
    schemaVersion: 1, version, platform: "windows-x86_64",
    compatibility: { host: nativeId, uiProtocol: 1, engineProtocol: 1 }, files,
    components: { controller: nativeId, ui: uiId, engine: files[1].sha256 },
  };
}

function treeHash(root) {
  const hash = createHash("sha256");
  function visit(relative) {
    for (const entry of fs.readdirSync(path.join(root, relative), { withFileTypes: true }).sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0)) {
      const next = relative ? `${relative}/${entry.name}` : entry.name;
      if (entry.isSymbolicLink()) throw new Error("Component tree contains a symlink");
      if (entry.isDirectory()) visit(next);
      else if (entry.isFile()) hash.update(next).update("\0").update(sha256(fs.readFileSync(path.join(root, next)))).update("\0");
      else throw new Error("Component tree contains a non-file entry");
    }
  }
  visit("");
  return hash.digest("hex");
}

if (require.main === module) {
  const [root, version, nativeId, renderer, output] = process.argv.slice(2);
  if (!output) throw new Error("Usage: node create-runtime-manifest.cjs <runtime> <version> <native-id> <renderer> <output>");
  const manifest = createManifest(root, version, nativeId, treeHash(renderer));
  fs.writeFileSync(output, `${JSON.stringify(manifest, null, 2)}\n`, { flag: "wx" });
}

module.exports = { FILES, BLOCK_SIZE, sha256, createManifest, treeHash };
