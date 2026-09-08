const { copyFileSync, existsSync, mkdirSync, statSync } = require("node:fs");
const { createHash } = require("node:crypto");
const { join, resolve } = require("node:path");
const { assertStandaloneRuntime } = require("./release/windows-runtime-imports.cjs");

const root = resolve(__dirname, "..");
const targetTriple = "x86_64-pc-windows-msvc";
const outputDirectory = join(root, "src-tauri", "binaries");

const sidecars = [
  {
    name: "clipture_engine",
    source: process.env.CLIPTURE_ENGINE_PATH || join(root, "build", "engine", "Release", "clipture_engine.exe")
  },
  {
    name: "ffmpeg",
    source: process.env.CLIPTURE_FFMPEG_PATH || join(root, "node_modules", "ffmpeg-static", "ffmpeg.exe")
  }
];

function sha256(filePath) {
  const hash = createHash("sha256");
  const { readFileSync } = require("node:fs");
  hash.update(readFileSync(filePath));
  return hash.digest("hex");
}

mkdirSync(outputDirectory, { recursive: true });

for (const sidecar of sidecars) {
  if (!existsSync(sidecar.source)) {
    throw new Error(`Missing ${sidecar.name} sidecar: ${sidecar.source}`);
  }

  assertStandaloneRuntime(sidecar.source);
  const destination = join(outputDirectory, `${sidecar.name}-${targetTriple}.exe`);
  const sourceHash = sha256(sidecar.source);
  const destinationHash = existsSync(destination) ? sha256(destination) : "";
  if (sourceHash !== destinationHash) copyFileSync(sidecar.source, destination);

  const sizeMiB = (statSync(destination).size / (1024 * 1024)).toFixed(2);
  process.stdout.write(`${sidecar.name}: ${destination} (${sizeMiB} MiB, sha256 ${sourceHash})\n`);
}
