#!/usr/bin/env node

const {
  createHash,
} = require("node:crypto");
const {
  createReadStream,
  existsSync,
  mkdirSync,
  readFileSync,
  renameSync,
  statSync,
  unlinkSync,
  writeFileSync,
} = require("node:fs");
const { basename, dirname, resolve } = require("node:path");

const VERSION_PATTERN = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/;
const WINDOWS_NSIS_KEYS = new Set(["windows-x86_64", "windows-x86_64-nsis"]);

function parseArguments(argv) {
  const options = new Map();
  for (let index = 0; index < argv.length; index += 2) {
    const key = argv[index];
    const value = argv[index + 1];
    if (!key?.startsWith("--") || value === undefined || value.startsWith("--")) {
      throw new Error(`Expected --name value arguments, received ${key ?? "<end>"}.`);
    }
    if (options.has(key)) throw new Error(`Duplicate argument: ${key}`);
    options.set(key, value);
  }

  const allowed = new Set([
    "--version",
    "--installer",
    "--asset-name",
    "--signature",
    "--latest-json",
    "--release-date",
    "--output",
  ]);
  for (const key of options.keys()) {
    if (!allowed.has(key)) throw new Error(`Unknown argument: ${key}`);
  }
  for (const required of ["--version", "--installer", "--release-date", "--output"]) {
    if (!options.has(required)) throw new Error(`Missing required argument: ${required}`);
  }

  return Object.fromEntries([...options].map(([key, value]) => [key.slice(2), value]));
}

function normalizeVersion(rawVersion) {
  const version = rawVersion.startsWith("v") ? rawVersion.slice(1) : rawVersion;
  if (!VERSION_PATTERN.test(version)) {
    throw new Error(`Expected a semantic version, received: ${rawVersion}`);
  }
  return version;
}

function normalizeReleaseDate(rawDate) {
  const epoch = Date.parse(rawDate);
  if (!Number.isFinite(epoch)) throw new Error(`Expected an RFC 3339 release date, received: ${rawDate}`);
  return new Date(epoch).toISOString();
}

function validateAssetName(rawName) {
  if (
    !rawName ||
    basename(rawName) !== rawName ||
    rawName === "." ||
    rawName === ".." ||
    /[\r\n:#?]/.test(rawName) ||
    !rawName.toLowerCase().endsWith(".exe")
  ) {
    throw new Error(`Unsafe or non-NSIS release asset name: ${rawName}`);
  }
  return rawName;
}

function requireRegularFile(filePath, label) {
  const absolutePath = resolve(filePath);
  const stat = statSync(absolutePath);
  if (!stat.isFile()) throw new Error(`${label} is not a regular file: ${absolutePath}`);
  return { absolutePath, stat };
}

function hashFileSha512(filePath) {
  return new Promise((resolveHash, reject) => {
    const hash = createHash("sha512");
    const input = createReadStream(filePath);
    input.on("data", (chunk) => hash.update(chunk));
    input.on("error", reject);
    input.on("end", () => resolveHash(hash.digest("base64")));
  });
}

function yamlSingleQuoted(value) {
  return `'${String(value).replaceAll("'", "''")}'`;
}

function renderLatestYml(metadata) {
  const asset = yamlSingleQuoted(metadata.assetName);
  return [
    `version: ${metadata.version}`,
    "files:",
    `  - url: ${asset}`,
    `    sha512: ${metadata.sha512}`,
    `    size: ${metadata.size}`,
    `path: ${asset}`,
    `sha512: ${metadata.sha512}`,
    `releaseDate: ${yamlSingleQuoted(metadata.releaseDate)}`,
    "",
  ].join("\n");
}

function validateSignature(signaturePath, installerPath) {
  if (!signaturePath) return undefined;
  const { absolutePath, stat } = requireRegularFile(signaturePath, "Updater signature");
  if (stat.size === 0) throw new Error(`Updater signature is empty: ${absolutePath}`);
  if (absolutePath !== `${installerPath}.sig`) {
    throw new Error(`Updater signature must be adjacent to and named after the NSIS installer.`);
  }
  const signature = readFileSync(absolutePath, "utf8").trim();
  if (!signature) throw new Error(`Updater signature is empty: ${absolutePath}`);
  return signature;
}

function validateLatestJson(latestJsonPath, version, assetName, expectedSignature) {
  if (!latestJsonPath) return;
  const { absolutePath } = requireRegularFile(latestJsonPath, "Tauri latest.json");
  const manifest = JSON.parse(readFileSync(absolutePath, "utf8"));
  if (normalizeVersion(String(manifest.version ?? "")) !== version) {
    throw new Error(`latest.json version does not match bridge version ${version}.`);
  }
  if (!manifest.platforms || typeof manifest.platforms !== "object") {
    throw new Error("latest.json has no platforms object.");
  }

  const candidates = Object.entries(manifest.platforms).filter(([key]) => WINDOWS_NSIS_KEYS.has(key));
  const exact = candidates.find(([, platform]) => {
    if (!platform || typeof platform !== "object") return false;
    if (typeof platform.signature !== "string" || platform.signature.trim() === "") return false;
    if (expectedSignature && platform.signature.trim() !== expectedSignature) return false;
    if (typeof platform.url !== "string") return false;
    try {
      return decodeURIComponent(new URL(platform.url).pathname.split("/").at(-1)) === assetName;
    } catch {
      return false;
    }
  });
  if (!exact) {
    throw new Error(`latest.json does not contain a signed Windows x64 NSIS URL for ${assetName}.`);
  }
}

function writeAtomically(outputPath, contents) {
  const absoluteOutput = resolve(outputPath);
  mkdirSync(dirname(absoluteOutput), { recursive: true });
  const temporary = `${absoluteOutput}.tmp-${process.pid}`;
  try {
    writeFileSync(temporary, contents, { encoding: "utf8", flag: "wx" });
    renameSync(temporary, absoluteOutput);
  } finally {
    if (existsSync(temporary)) unlinkSync(temporary);
  }
  return absoluteOutput;
}

async function createElectronBridge(options) {
  const version = normalizeVersion(options.version);
  const { absolutePath: installerPath, stat } = requireRegularFile(options.installer, "NSIS installer");
  const assetName = validateAssetName(options["asset-name"] ?? basename(installerPath));
  const releaseDate = normalizeReleaseDate(options["release-date"]);
  const signature = validateSignature(options.signature, installerPath);
  validateLatestJson(options["latest-json"], version, assetName, signature);

  const sha512 = await hashFileSha512(installerPath);
  const metadata = { version, assetName, sha512, size: stat.size, releaseDate };
  const outputPath = writeAtomically(options.output, renderLatestYml(metadata));
  return { ...metadata, outputPath };
}

async function main() {
  const result = await createElectronBridge(parseArguments(process.argv.slice(2)));
  console.log(`Created Electron cross-grade metadata for ${result.assetName} at ${result.outputPath}.`);
}

if (require.main === module) {
  main().catch((error) => {
    console.error(error instanceof Error ? error.message : error);
    process.exitCode = 1;
  });
}

module.exports = {
  createElectronBridge,
  normalizeReleaseDate,
  normalizeVersion,
  parseArguments,
  renderLatestYml,
  validateAssetName,
  validateLatestJson,
};
