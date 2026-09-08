// Read-only drift check for the two-call patch to the pinned upstream template.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const root = path.resolve(__dirname, '../..');
const pkg = require('../../package.json');
const config = require('../../src-tauri/tauri.conf.json');
assert.equal(pkg.devDependencies['@tauri-apps/cli'], '2.11.4', 'Rebase/test the installer template when updating Tauri CLI');
assert.equal(config.bundle.windows.nsis.template, 'installer/tauri-2.11.4.nsi');
const template = fs.readFileSync(path.join(root, 'src-tauri/installer/tauri-2.11.4.nsi'), 'utf8').replace(/\r\n/g, '\n');
const upstream = template.split('\n').slice(3).join('\n')
  .replace('  !insertmacro CLIPTURE_INIT_BEGIN\n', '')
  .replace('  !insertmacro CLIPTURE_INIT_END\n', '');
assert.equal(createHash('sha256').update(upstream).digest('hex'),
  '20f4ecc730defb71f1342eaeaec4021df13be3d843abba0effe88ea5835fa079',
  'Keep product logic in hooks.nsh, not in the vendored template');
const start = template.indexOf('Function .onInit');
const end = template.indexOf('FunctionEnd', start);
const init = template.slice(start, end);
assert.ok(init.indexOf('CLIPTURE_INIT_BEGIN') < init.indexOf('MULTIUSER_INIT'));
assert.ok(init.indexOf('CLIPTURE_INIT_END') > init.indexOf('MULTIUSER_INIT'));
console.log('Pinned installer template: only the two expected init hooks differ.');
