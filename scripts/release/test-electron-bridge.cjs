#!/usr/bin/env node

const assert = require("node:assert/strict");
const { mkdtempSync, readFileSync, rmSync, writeFileSync } = require("node:fs");
const { tmpdir } = require("node:os");
const { join, resolve } = require("node:path");
const { spawnSync } = require("node:child_process");

const {
  validateAssetName,
  validateLatestJson,
} = require("./create-electron-bridge.cjs");

const sandbox = mkdtempSync(join(tmpdir(), "clipture-electron-bridge-"));
try {
  const assetName = "Clipture_9.8.7_x64-setup.exe";
  const installer = join(sandbox, assetName);
  const signature = `${installer}.sig`;
  const latestJson = join(sandbox, "latest.json");
  const output = join(sandbox, "metadata", "latest.yml");
  writeFileSync(installer, "tauri-nsis-fixture\n");
  writeFileSync(signature, "trusted updater signature\n");
  writeFileSync(
    latestJson,
    JSON.stringify({
      version: "9.8.7",
      platforms: {
        "windows-x86_64-nsis": {
          signature: "trusted updater signature",
          url: `https://github.com/Chronyyx/Clipture/releases/download/v9.8.7/${assetName}`,
        },
      },
    }),
  );

  const script = resolve(__dirname, "create-electron-bridge.cjs");
  const args = [
    script,
    "--version", "v9.8.7",
    "--installer", installer,
    "--asset-name", assetName,
    "--signature", signature,
    "--latest-json", latestJson,
    "--release-date", "2026-09-04T01:02:03-04:00",
    "--output", output,
  ];
  const first = spawnSync(process.execPath, args, { encoding: "utf8" });
  assert.equal(first.status, 0, first.stderr);

  const expected = [
    "version: 9.8.7",
    "files:",
    `  - url: '${assetName}'`,
    "    sha512: /QcyAvTvtwTVu4PD3W4yjhb7VEJE3DiPZiEph3M+o/Yfjn7uDn0v2PtQ3QLUE/Izti5mkEm+P3iQgpDnaqQClg==",
    "    size: 19",
    `path: '${assetName}'`,
    "sha512: /QcyAvTvtwTVu4PD3W4yjhb7VEJE3DiPZiEph3M+o/Yfjn7uDn0v2PtQ3QLUE/Izti5mkEm+P3iQgpDnaqQClg==",
    "releaseDate: '2026-09-04T05:02:03.000Z'",
    "",
  ].join("\n");
  assert.equal(readFileSync(output, "utf8"), expected);
  assert.ok(!readFileSync(output, "utf8").includes(sandbox), "metadata leaked a local path");

  const second = spawnSync(process.execPath, args, { encoding: "utf8" });
  assert.equal(second.status, 0, second.stderr);
  assert.equal(readFileSync(output, "utf8"), expected, "rerun was not deterministic");

  assert.throws(() => validateAssetName("../Clipture.exe"), /Unsafe/);
  const unsignedJson = join(sandbox, "unsigned.json");
  writeFileSync(unsignedJson, JSON.stringify({
    version: "9.8.7",
    platforms: {
      "windows-x86_64": { signature: "", url: `https://example.test/${assetName}` },
    },
  }));
  assert.throws(
    () => validateLatestJson(unsignedJson, "9.8.7", assetName),
    /signed Windows x64 NSIS/,
  );

  assert.throws(
    () => validateLatestJson(latestJson, "9.8.7", assetName, "different signature"),
    /signed Windows x64 NSIS/,
  );

  console.log("Electron cross-grade metadata tests passed.");
} finally {
  rmSync(sandbox, { recursive: true, force: true });
}
