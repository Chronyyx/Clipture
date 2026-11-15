// Provides the pinned FFmpeg build bundled with Clipture. The archive and every
// file taken from it are checked against pinned SHA-256 hashes, so a changed
// download (or a tampered cache) fails the build instead of shipping.
// Usage: node scripts/fetch-ffmpeg.cjs  (prints the verified ffmpeg.exe path)
const { createHash } = require("node:crypto");
const { existsSync, mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } = require("node:fs");
const { spawnSync } = require("node:child_process");
const { join, resolve } = require("node:path");

// gyan.dev "essentials" release, the same build family as the previous
// ffmpeg-static binary. To update: change every value together, after
// checking the archive hash against gyan.dev's published .sha256. The build
// is GPL-3.0; its LICENSE and README.txt (source commit and configuration)
// ship with the installer as its notices (THIRD_PARTY_NOTICES.md).
const FFMPEG = {
  version: "9.0.2",
  url: "https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip",
  archiveSha256: "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba",
  executableSha256: "3256173f3f8bffd7df12227c68adf68025edb1832273a9530688a7bb1ed8edec",
  licenseSha256: "8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903",
  readmeSha256: "0342de6bb39dd421cbec2c00d89ad274b640d75ad3f2cab09ffd647ce2d8f949"
};

const root = resolve(__dirname, "..");
const cacheDirectory = join(root, "build", "ffmpeg", FFMPEG.version);
const executable = join(cacheDirectory, "ffmpeg.exe");
// Archive member -> cached file and its pinned hash.
const MEMBERS = {
  "bin/ffmpeg.exe": { path: executable, sha256: FFMPEG.executableSha256 },
  LICENSE: { path: join(cacheDirectory, "FFmpeg-LICENSE.txt"), sha256: FFMPEG.licenseSha256 },
  "README.txt": { path: join(cacheDirectory, "FFmpeg-README.txt"), sha256: FFMPEG.readmeSha256 }
};

const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

function verified(path, expected) {
  return existsSync(path) && sha256(readFileSync(path)) === expected;
}

async function ensureArchive() {
  mkdirSync(cacheDirectory, { recursive: true });
  const archive = join(cacheDirectory, "ffmpeg.zip");
  if (verified(archive, FFMPEG.archiveSha256)) return archive;
  const response = await fetch(FFMPEG.url, { redirect: "follow" });
  if (!response.ok) throw new Error(`FFmpeg download failed: HTTP ${response.status}`);
  const bytes = Buffer.from(await response.arrayBuffer());
  const actual = sha256(bytes);
  if (actual !== FFMPEG.archiveSha256) {
    throw new Error(`FFmpeg archive hash mismatch: expected ${FFMPEG.archiveSha256}, got ${actual}`);
  }
  writeFileSync(archive, bytes);
  return archive;
}

// Extracts the archive members whose cached copies are missing or changed.
async function ensureMembers(names) {
  const missing = names.filter((name) => !verified(MEMBERS[name].path, MEMBERS[name].sha256));
  if (missing.length) {
    const archive = await ensureArchive();
    // Windows' own bsdtar reads zip archives; Git's GNU tar on PATH does not.
    const tar = join(process.env.SystemRoot || "C:\\Windows", "System32", "tar.exe");
    const extracted = join(cacheDirectory, "extract");
    rmSync(extracted, { recursive: true, force: true });
    mkdirSync(extracted, { recursive: true });
    const prefix = `ffmpeg-${FFMPEG.version}-essentials_build/`;
    const result = spawnSync(tar, ["-xf", archive, "-C", extracted, ...missing.map((name) => prefix + name)],
      { stdio: "inherit" });
    if (result.status !== 0) throw new Error("FFmpeg archive extraction failed");
    for (const name of missing) {
      const candidate = join(extracted, ...(prefix + name).split("/"));
      if (!verified(candidate, MEMBERS[name].sha256)) {
        throw new Error(`Extracted FFmpeg ${name} does not match the pinned hash`);
      }
      renameSync(candidate, MEMBERS[name].path);
    }
    rmSync(extracted, { recursive: true, force: true });
  }
  return names.map((name) => MEMBERS[name].path);
}

async function ensureFfmpeg() {
  const [path] = await ensureMembers(["bin/ffmpeg.exe"]);
  return path;
}

// The build's GPL-3.0 license text and its README (source commit, configuration).
async function ensureFfmpegNotices() {
  const [license, readme] = await ensureMembers(["LICENSE", "README.txt"]);
  return { license, readme };
}

module.exports = { FFMPEG, ensureFfmpeg, ensureFfmpegNotices };

if (require.main === module) {
  ensureFfmpeg().then(
    (path) => process.stdout.write(`${path}\n`),
    (error) => {
      process.stderr.write(`${error.message}\n`);
      process.exit(1);
    }
  );
}
