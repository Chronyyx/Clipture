// Provides the pinned FFmpeg build bundled with Clipture. The archive and the
// extracted executable are both checked against pinned SHA-256 hashes, so a
// changed download (or a tampered cache) fails the build instead of shipping.
// Usage: node scripts/fetch-ffmpeg.cjs  (prints the verified ffmpeg.exe path)
const { createHash } = require("node:crypto");
const { existsSync, mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } = require("node:fs");
const { spawnSync } = require("node:child_process");
const { join, resolve } = require("node:path");

// gyan.dev "essentials" release, the same build family as the previous
// ffmpeg-static binary. To update: change all four values together, after
// checking the archive hash against gyan.dev's published .sha256.
const FFMPEG = {
  version: "9.0.2",
  url: "https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip",
  archiveSha256: "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba",
  executableSha256: "3256173f3f8bffd7df12227c68adf68025edb1832273a9530688a7bb1ed8edec"
};

const root = resolve(__dirname, "..");
const cacheDirectory = join(root, "build", "ffmpeg", FFMPEG.version);
const executable = join(cacheDirectory, "ffmpeg.exe");

const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

function verified(path, expected) {
  return existsSync(path) && sha256(readFileSync(path)) === expected;
}

async function ensureFfmpeg() {
  if (verified(executable, FFMPEG.executableSha256)) return executable;
  mkdirSync(cacheDirectory, { recursive: true });

  const archive = join(cacheDirectory, "ffmpeg.zip");
  if (!verified(archive, FFMPEG.archiveSha256)) {
    const response = await fetch(FFMPEG.url, { redirect: "follow" });
    if (!response.ok) throw new Error(`FFmpeg download failed: HTTP ${response.status}`);
    const bytes = Buffer.from(await response.arrayBuffer());
    const actual = sha256(bytes);
    if (actual !== FFMPEG.archiveSha256) {
      throw new Error(`FFmpeg archive hash mismatch: expected ${FFMPEG.archiveSha256}, got ${actual}`);
    }
    writeFileSync(archive, bytes);
  }

  // Windows' own bsdtar reads zip archives; Git's GNU tar on PATH does not.
  const tar = join(process.env.SystemRoot || "C:\\Windows", "System32", "tar.exe");
  const extracted = join(cacheDirectory, "extract");
  rmSync(extracted, { recursive: true, force: true });
  mkdirSync(extracted, { recursive: true });
  const member = `ffmpeg-${FFMPEG.version}-essentials_build/bin/ffmpeg.exe`;
  const result = spawnSync(tar, ["-xf", archive, "-C", extracted, member], { stdio: "inherit" });
  if (result.status !== 0) throw new Error("FFmpeg archive extraction failed");
  const candidate = join(extracted, ...member.split("/"));
  if (!verified(candidate, FFMPEG.executableSha256)) {
    throw new Error("Extracted ffmpeg.exe does not match the pinned hash");
  }
  renameSync(candidate, executable);
  rmSync(extracted, { recursive: true, force: true });
  return executable;
}

module.exports = { FFMPEG, ensureFfmpeg };

if (require.main === module) {
  ensureFfmpeg().then(
    (path) => process.stdout.write(`${path}\n`),
    (error) => {
      process.stderr.write(`${error.message}\n`);
      process.exit(1);
    }
  );
}
