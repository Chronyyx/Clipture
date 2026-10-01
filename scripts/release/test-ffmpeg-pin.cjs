// Read-only: the FFmpeg pin is well-formed, points at the expected builder,
// and any staged or cached binary matches it. Never downloads anything.
const assert = require("node:assert/strict");
const { createHash } = require("node:crypto");
const { existsSync, readFileSync } = require("node:fs");
const { join, resolve } = require("node:path");
const { FFMPEG } = require("../fetch-ffmpeg.cjs");

const root = resolve(__dirname, "..", "..");
assert.match(FFMPEG.version, /^\d+\.\d+(\.\d+)?$/);
assert.match(FFMPEG.archiveSha256, /^[a-f0-9]{64}$/);
assert.match(FFMPEG.executableSha256, /^[a-f0-9]{64}$/);
assert.equal(
  FFMPEG.url,
  `https://github.com/GyanD/codexffmpeg/releases/download/${FFMPEG.version}/ffmpeg-${FFMPEG.version}-essentials_build.zip`
);

const sha256 = (path) => createHash("sha256").update(readFileSync(path)).digest("hex");
let checked = 0;
for (const path of [
  join(root, "build", "ffmpeg", FFMPEG.version, "ffmpeg.exe"),
  join(root, "src-tauri", "binaries", "ffmpeg-x86_64-pc-windows-msvc.exe")
]) {
  if (!existsSync(path) || process.env.CLIPTURE_FFMPEG_PATH) continue;
  assert.equal(sha256(path), FFMPEG.executableSha256, `${path} does not match the pinned FFmpeg ${FFMPEG.version}`);
  checked += 1;
}
console.log(`FFmpeg pin ${FFMPEG.version}: well-formed; ${checked} present binaries match.`);
