// Collects the license notices of every third-party package compiled into what
// Clipture ships: the Rust crates linked into clipture.exe (normal, non-dev
// dependencies for x86_64-pc-windows-msvc) and the npm packages bundled into
// the renderer (production dependencies). Writes one text file with each
// package's name, version, declared license and license/notice texts.
// Fails when a package's license is copyleft (GPL, LGPL, AGPL, MPL, EPL, CDDL)
// or unknown, so a new dependency cannot change the release terms unnoticed.
// Usage: node scripts/release/collect-dependency-licenses.cjs <output file>
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const root = path.resolve(__dirname, '../..');
const output = process.argv[2] || path.join(root, 'build/notices/DEPENDENCY-LICENSES.txt');
const NOTICE = /^(licen[cs]e|copying|notice|copyright|unlicense)([-_.].*)?$/i;

function noticeTexts(directory) {
  if (!directory || !fs.existsSync(directory)) return [];
  return fs.readdirSync(directory).filter((name) => NOTICE.test(name) && fs.statSync(path.join(directory, name)).isFile())
    .sort().map((name) => ({ name, text: fs.readFileSync(path.join(directory, name), 'utf8').trim() }));
}

function rustPackages() {
  const metadata = JSON.parse(execFileSync('cargo', ['metadata', '--format-version', '1', '--locked',
    '--filter-platform', 'x86_64-pc-windows-msvc', '--manifest-path', path.join(root, 'src-tauri/Cargo.toml')],
    { encoding: 'utf8', maxBuffer: 256 * 1024 * 1024 }));
  const byId = new Map(metadata.packages.map((p) => [p.id, p]));
  const nodes = new Map(metadata.resolve.nodes.map((n) => [n.id, n]));
  const seen = new Set();
  // Normal dependencies only: build scripts and dev dependencies do not ship.
  const walk = (id) => {
    if (seen.has(id)) return;
    seen.add(id);
    for (const dep of nodes.get(id)?.deps || []) {
      if (dep.dep_kinds.some((kind) => kind.kind === null)) walk(dep.pkg);
    }
  };
  walk(metadata.resolve.root);
  seen.delete(metadata.resolve.root);
  return [...seen].map((id) => byId.get(id)).map((p) => ({ ecosystem: 'Rust crate', name: p.name, version: p.version,
    license: p.license || (p.license_file ? `see ${p.license_file}` : ''), directory: path.dirname(p.manifest_path) }));
}

function npmPackages() {
  const tree = JSON.parse(execFileSync(process.platform === 'win32' ? 'npm.cmd' : 'npm',
    ['ls', '--omit=dev', '--all', '--json', '--long'], { cwd: root, encoding: 'utf8', shell: process.platform === 'win32',
      maxBuffer: 256 * 1024 * 1024 }));
  const found = new Map();
  const walk = (dependencies) => {
    for (const [name, info] of Object.entries(dependencies || {})) {
      if (!info.version || found.has(`${name}@${info.version}`)) continue;
      const license = typeof info.license === 'string' ? info.license : info.license?.type || '';
      found.set(`${name}@${info.version}`, { ecosystem: 'npm package', name, version: info.version, license,
        directory: info.path });
      walk(info.dependencies);
    }
  };
  // Only packages the renderer imports are bundled into what ships; the rest of
  // package.json's dependencies serve the Electron reference host or
  // development (ffmpeg-static: the shipped ffmpeg.exe is the pinned build).
  const rendererSources = execFileSync('git', ['grep', '-h', '-o', '-E', `from ['"][^'"./][^'"]*['"]|import\\(['"][^'"./][^'"]*['"]\\)|@import ['"][^'"./][^'"]*['"]`,
    '--', 'src/renderer', 'src/shared'], { cwd: root, encoding: 'utf8' });
  const imported = new Set([...rendererSources.matchAll(/['"]((?:@[^/'"]+\/)?[^/'"]+)/g)].map((m) => m[1]));
  walk(Object.fromEntries(Object.entries(tree.dependencies || {}).filter(([name]) => imported.has(name))));
  return [...found.values()];
}

const packages = [...rustPackages(), ...npmPackages()].sort((a, b) =>
  a.ecosystem.localeCompare(b.ecosystem) || a.name.localeCompare(b.name) || a.version.localeCompare(b.version));
// Permissive identifiers: one alternative made only of these satisfies a package.
const PERMISSIVE = new Set(['MIT', 'MIT-0', 'APACHE-2.0', 'APACHE-2.0 WITH LLVM-EXCEPTION', 'BSD-2-CLAUSE', 'BSD-3-CLAUSE',
  '0BSD', 'ISC', 'ZLIB', 'UNLICENSE', 'BSL-1.0', 'CC0-1.0', 'UNICODE-3.0', 'UNICODE-DFS-2016', 'CDLA-PERMISSIVE-2.0',
  'BLUEOAK-1.0.0', 'PYTHON-2.0', 'CC-BY-4.0', 'WTFPL']);
function permissiveChoice(expression) {
  if (!expression) return false;
  const alternatives = expression.replace(/[()]/g, ' ').split(/\s+OR\s+|\//i);
  return alternatives.some((alternative) => alternative.split(/\s+AND\s+/i)
    .every((id) => PERMISSIVE.has(id.trim().toUpperCase())));
}
// Reviewed and accepted, with what each one asks of a binary distribution:
// - MPL-2.0 (file-level copyleft): may ship inside a larger work under any
//   license; the crates are unmodified and their source is on crates.io.
// - OFL-1.1 (fonts): may be bundled with software; the license ships with them.
const ACCEPTED = { 'MPL-2.0': 'unmodified; source at https://crates.io/crates/', 'OFL-1.1': 'font, bundled with its license' };
const accepted = (p) => Object.keys(ACCEPTED).includes((p.license || '').trim());
const review = packages.filter((p) => !permissiveChoice(p.license));
// Anything else without a permissive alternative waits for a decision before it ships.
const blocking = review.filter((p) => !accepted(p));

const lines = [
  'Third-party software compiled into Clipture',
  '',
  'Clipture (MIT with the Commons Clause License Condition v1.0) includes the',
  'packages below. Each remains under its own license; the Commons Clause does',
  'not apply to them. A package offered under several licenses is used under',
  'the first permissive one listed. ffmpeg.exe is a separate program; see',
  'THIRD_PARTY_NOTICES.md and FFmpeg-LICENSE.txt.',
  '',
  `${packages.length} packages.`,
  '',
];
for (const p of packages) {
  lines.push('='.repeat(78), `${p.name} ${p.version} (${p.ecosystem})`, `License: ${p.license || 'not declared'}`, '');
  const texts = noticeTexts(p.directory);
  if (accepted(p)) lines.push(`Accepted: ${ACCEPTED[p.license.trim()]}${p.license.trim() === 'MPL-2.0' ? `${p.name}/${p.version}` : ''}`, '');
  if (!texts.length) lines.push(`(No license file in the published package; its declared license is ${p.license || 'not declared'}.)`, '');
  for (const { name, text } of texts) lines.push(`--- ${name} ---`, text, '');
}
fs.mkdirSync(path.dirname(output), { recursive: true });
fs.writeFileSync(output, lines.join('\n') + '\n');
console.log(`${packages.length} packages (${packages.filter((p) => p.ecosystem === 'Rust crate').length} Rust, ` +
  `${packages.filter((p) => p.ecosystem === 'npm package').length} npm) -> ${output}`);
for (const p of review) console.log(`${accepted(p) ? 'accepted' : 'review'}: ${p.name} ${p.version} (${p.ecosystem}): ${p.license || 'no license declared'}`);
if (blocking.length) {
  console.error(`Licenses that need a decision before release: ${blocking.map((p) => `${p.name} ${p.version}`).join(', ')}`);
  process.exit(1);
}
