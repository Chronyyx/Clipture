'use strict';
// clipture.app as rendered for release: the pages, search and share files
// ship, the demo page gets its own title and card, and a bare /demo/ opens
// the demo rather than an empty, disconnected app. Writes only to a temp dir.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '../..');
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'clipture-site-'));
try {
  const demo = path.join(work, 'demo-build');
  fs.mkdirSync(demo);
  fs.writeFileSync(path.join(demo, 'index.html'), '<!doctype html><html lang="en"><head><title>Clipture</title></head><body><div id="root"></div></body></html>');
  const output = path.join(work, 'site');
  execFileSync(process.execPath, [path.join(__dirname, 'render-site.cjs'), path.join(root, 'web'), output, '--demo', demo], { stdio: 'pipe' });
  const read = (file) => fs.readFileSync(path.join(output, file), 'utf8');

  for (const file of ['index.html', '404.html', 'robots.txt', 'sitemap.xml', 'og-image.png', '_headers']) {
    assert.ok(fs.existsSync(path.join(output, file)), `${file} ships`);
  }
  const home = read('index.html');
  for (const tag of ['rel="canonical"', 'property="og:title"', 'property="og:image"', 'name="twitter:card"']) {
    assert.ok(home.includes(tag), `home has ${tag}`);
  }
  assert.match(home, /class="skip-link"[^>]*>Skip to content</, 'the first stop skips to the content');
  assert.match(home, /Skip the demo/, 'Tab can get past the embedded app');

  const missing = read('404.html');
  assert.match(missing, /<html lang="en">/);
  assert.match(missing, /<title>[^<]+<\/title>/);
  assert.equal((missing.match(/<h1[\s>]/g) || []).length, 1, 'the 404 has one heading');
  assert.match(missing, /href="\/"/, 'the 404 links home');

  assert.match(read('robots.txt'), /Sitemap: https:\/\/clipture\.app\/sitemap\.xml/);
  assert.match(read('sitemap.xml'), /<loc>https:\/\/clipture\.app\/<\/loc>/);
  // /demo/ is served with X-Robots-Tag: noindex; listing it would contradict that.
  assert.doesNotMatch(read('sitemap.xml'), /\/demo\//, 'the noindexed demo stays out of the sitemap');
  // Every sitemap page ships, links back home, and is linked from the home page.
  for (const [, url] of read('sitemap.xml').matchAll(/<loc>https:\/\/clipture\.app(\/[^<]*)<\/loc>/g)) {
    if (url === '/') continue;
    const page = read(path.join(url, 'index.html'));
    assert.match(page, new RegExp(`<link rel="canonical" href="https://clipture\\.app${url}">`), `${url} is canonical`);
    assert.equal((page.match(/<h1[\s>]/g) || []).length, 1, `${url} has one heading`);
    assert.ok(home.includes(`href="${url}"`), `the home page links ${url}`);
  }
  const version = require(path.join(root, 'package.json')).version;
  assert.match(home, new RegExp(`"softwareVersion": "${version.replace(/\./g, '\\.')}"`), 'structured data names the release');
  const png = fs.readFileSync(path.join(output, 'og-image.png'));
  assert.deepEqual([png.readUInt32BE(16), png.readUInt32BE(20)], [1200, 630], 'the share image is 1200x630');

  const demoPage = read('demo/index.html');
  assert.doesNotMatch(demoPage, /<title>Clipture<\/title>/);
  assert.match(demoPage, /<title>Clipture live demo/);
  assert.match(demoPage, /name="description"/);
  assert.match(demoPage, /property="og:image"/);
  console.log('Rendered site: 404, robots, sitemap, 1200x630 share card, skip links and demo metadata.');
} finally {
  fs.rmSync(work, { recursive: true, force: true });
}

// previewSearch, with the build flag Vite would define.
const source = ts.transpileModule(fs.readFileSync(path.join(root, 'src/renderer/platform/mockPreview.ts'), 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 }
}).outputText;
const previewSearchFor = (siteDemo) => {
  const exports = {};
  new Function('exports', 'require', '__SITE_DEMO__', source)(exports, () => ({ defaultSettings: {} }), siteDemo);
  return exports.previewSearch;
};
const site = previewSearchFor(true);
assert.equal(site(''), '?preview&demo', 'a bare /demo/ on clipture.app opens the demo');
assert.equal(site('?theme=light'), '?theme=light&preview&demo', 'other parameters are kept');
assert.equal(site('?preview&theme=aero'), '?preview&theme=aero', 'an explicit preview is left alone');
assert.equal(previewSearchFor(false)(''), '', 'the desktop and dev builds never turn preview on');
assert.equal(previewSearchFor(undefined)(''), '', 'an undefined flag counts as off');
console.log('Preview search: bare /demo/ opens the demo on the site build only.');
