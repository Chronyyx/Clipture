const { readFileSync, writeFileSync } = require("node:fs");
const { resolve } = require("node:path");

const tag = process.argv[2];
const outputPath = process.argv[3] ?? "release-notes.md";

if (!tag || !/^v\d+\.\d+\.\d+$/.test(tag)) {
  throw new Error(`Expected a semantic release tag such as v1.2.3, received: ${tag ?? "<missing>"}`);
}

const version = tag.slice(1);
const packageJson = JSON.parse(readFileSync(resolve("package.json"), "utf8"));
if (packageJson.version !== version) {
  throw new Error(`Release tag ${tag} does not match package.json version ${packageJson.version}.`);
}

const changelog = readFileSync(resolve("CHANGELOG.md"), "utf8");
const escapedVersion = version.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const heading = new RegExp(`^## \\[${escapedVersion}\\].*$`, "m");
const match = heading.exec(changelog);
if (!match || match.index === undefined) {
  throw new Error(`CHANGELOG.md has no section for ${version}.`);
}

const following = changelog.slice(match.index + match[0].length);
const nextHeading = following.search(/^## /m);
const body = `${match[0]}${nextHeading >= 0 ? following.slice(0, nextHeading) : following}`.trim();
if (!body) throw new Error(`The changelog section for ${version} is empty.`);

writeFileSync(resolve(outputPath), `${body}\n`, "utf8");
console.log(`Prepared release notes for ${tag} at ${outputPath}.`);
