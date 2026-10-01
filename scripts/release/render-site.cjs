'use strict';
// Assembles clipture.app: copies web/, optionally adds the real Clipture
// interface built in demo mode at /demo/, and points download links at the
// release's signed installer. Unrendered pages still work (they link to
// releases/latest), so web/ can be previewed without a build.
//
// Usage: node scripts/release/render-site.cjs web dist/site [--demo dist/site-demo]
// CLIPTURE_VERSION overrides the version from package.json (release builds).
// Read-only on the repository; writes only to the output directory.
const fs = require('node:fs');
const path = require('node:path');

const args = process.argv.slice(2);
const demoIndex = args.indexOf('--demo');
const demo = demoIndex >= 0 ? args.splice(demoIndex, 2)[1] : undefined;
const [source = 'web', output = 'dist/site'] = args;

const packageVersion = require(path.join(__dirname, '..', '..', 'package.json')).version;
const version = (process.env.CLIPTURE_VERSION || packageVersion || '').replace(/^v/, '');
if (!/^\d+\.\d+\.\d+$/.test(version)) {
  throw new Error(`CLIPTURE_VERSION must be a release version like 1.5.4 (got "${version}")`);
}

const repository = 'https://github.com/Chronyyx/Clipture';
const latest = `${repository}/releases/latest`;
const installer = `${repository}/releases/download/v${version}/Clipture_${version}_x64-setup.exe`;

// Each rule must match at least once in the files it names, so a renamed
// button cannot silently ship an unversioned site.
const rules = [
  { files: ['index.html', 'invite/index.html'], from: `href="${latest}"`, to: `href="${installer}"` },
  { files: ['index.html'], from: '>Download for Windows<', to: `>Download Clipture ${version}<` },
  { files: ['invite/index.html'], from: '>the releases page<', to: `>download Clipture ${version}<` }
];

fs.rmSync(output, { recursive: true, force: true });
fs.cpSync(source, output, { recursive: true });

for (const rule of rules) {
  for (const file of rule.files) {
    const target = path.join(output, file);
    const text = fs.readFileSync(target, 'utf8');
    if (!text.includes(rule.from)) {
      throw new Error(`${file}: expected to find ${rule.from}`);
    }
    fs.writeFileSync(target, text.split(rule.from).join(rule.to));
  }
}

// Stamp local CSS, JS and image links with a content hash. Cloudflare lets
// browsers keep these for hours, so a changed file must get a new URL.
const crypto = require('node:crypto');
const htmlFiles = (dir) => fs.readdirSync(dir, { withFileTypes: true }).flatMap((entry) => {
  const full = path.join(dir, entry.name);
  return entry.isDirectory() ? htmlFiles(full) : entry.name.endsWith('.html') ? [full] : [];
});
for (const file of htmlFiles(output)) {
  const html = fs.readFileSync(file, 'utf8').replace(/(href|src)="(\/[^"?#]+\.(?:css|js|png))"/g, (match, attribute, url) => {
    const asset = path.join(output, url);
    if (!fs.existsSync(asset)) return match;
    const hash = crypto.createHash('sha256').update(fs.readFileSync(asset)).digest('hex').slice(0, 10);
    return `${attribute}="${url}?v=${hash}"`;
  });
  fs.writeFileSync(file, html);
}

if (demo) {
  if (!fs.existsSync(path.join(demo, 'index.html'))) {
    throw new Error(`${demo}: no index.html; build the renderer first (npm run build:site)`);
  }
  fs.cpSync(demo, path.join(output, 'demo'), { recursive: true });
}

console.log(`Rendered ${source} -> ${output} for Clipture ${version}${demo ? ' with the live demo at /demo/' : ''}`);
