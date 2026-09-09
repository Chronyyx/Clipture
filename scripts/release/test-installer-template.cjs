// Read-only drift check for the product hook calls in the pinned template.
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
  .replace('  !insertmacro CLIPTURE_INIT_END\n', '')
  .replace('!insertmacro CLIPTURE_STARTUP_PAGE\n', '')
  .replace('!insertmacro CLIPTURE_FINISH_SETUP\n', '')
  .replace('!insertmacro CLIPTURE_FINISH_ACTION\n', '')
  .replace('!insertmacro CLIPTURE_PROGRESS_STYLE ""\n', '')
  .replace('!insertmacro CLIPTURE_PROGRESS_STYLE "un."\n', '');
assert.equal(createHash('sha256').update(upstream).digest('hex'),
  '20f4ecc730defb71f1342eaeaec4021df13be3d843abba0effe88ea5835fa079',
  'Keep product logic in hooks.nsh, not in the vendored template');
const start = template.indexOf('Function .onInit');
const end = template.indexOf('FunctionEnd', start);
const init = template.slice(start, end);
assert.ok(init.indexOf('CLIPTURE_INIT_BEGIN') < init.indexOf('MULTIUSER_INIT'));
assert.ok(init.indexOf('CLIPTURE_INIT_END') > init.indexOf('MULTIUSER_INIT'));
assert.ok(template.indexOf('CLIPTURE_STARTUP_PAGE') < template.indexOf('!insertmacro MUI_PAGE_INSTFILES'));
assert.ok(template.indexOf('CLIPTURE_FINISH_SETUP') < template.indexOf('!insertmacro MUI_PAGE_FINISH'));
assert.ok(template.indexOf('CLIPTURE_FINISH_ACTION') > template.indexOf('!insertmacro MUI_PAGE_FINISH'));
const hooks = fs.readFileSync(path.join(root, 'src-tauri/installer/hooks.nsh'), 'utf8');
assert.match(hooks, /StrCpy \$CliptureStartupChoice \$\{BST_CHECKED\}/);
assert.match(hooks, /\$\{Silent\}[\s\S]*?\$PassiveMode = 1[\s\S]*?\$UpdateMode = 1[\s\S]*?Abort/);
assert.match(hooks, /RunAsUser.*\$1/);
assert.match(hooks, /NSD_Uncheck.*mui\.FinishPage\.Run/);
assert.match(hooks, /!macro NSIS_HOOK_POSTINSTALL\s+!insertmacro CLIPTURE_REFRESH_ICONS/,
  'Refresh on every install, including silent updates, before launching the app');
const refresh = fs.readFileSync(path.join(root, 'src-tauri/installer/icon-refresh.nsh'), 'utf8');
assert.match(refresh, /SHChangeNotify\(i 0x2000, i 0x3005, w/,
  'Targeted UPDATEITEM notification with Unicode path and FLUSHNOWAIT');
assert.match(refresh, /IsShortcutTarget[^\n]+\$INSTDIR\\\$\{MAINBINARYNAME\}\.exe/);
assert.match(refresh, /\$3 == 1[\s\S]*?CLIPTURE_NOTIFY_ICON/, 'Only matching shortcuts are refreshed');
assert.doesNotMatch(refresh, /^\s*(?:Delete|Exec|CreateShortCut|WriteReg|SetShellVarContext)\b/im,
  'Refresh must not rewrite pins, clear caches, run processes or change install scope');
assert.ok(template.indexOf('CLIPTURE_PROGRESS_STYLE ""') < template.indexOf('!insertmacro MUI_PAGE_INSTFILES'));
assert.ok(template.indexOf('CLIPTURE_PROGRESS_STYLE "un."') < template.indexOf('!insertmacro MUI_UNPAGE_INSTFILES'));
const nsis = config.bundle.windows.nsis;
for (const property of ['installerIcon', 'uninstallerIcon', 'sidebarImage', 'headerImage']) {
  assert.ok(fs.existsSync(path.resolve(root, 'src-tauri', nsis[property])), `${property} asset must exist`);
}
const theme = fs.readFileSync(path.join(root, 'src-tauri/installer/theme.nsh'), 'utf8');
assert.match(theme, /MUI_BGCOLOR "F0F0EB"/);
assert.match(theme, /PBM_SETBARCOLOR: orange/);
assert.doesNotMatch(theme, /(?:WriteReg|DeleteReg|ExecWait|RunAsUser)/, 'Theme must not own installation behavior');
const drawing = fs.readFileSync(path.join(root, 'scripts/release/build-installer-artwork.ps1'), 'utf8');
assert.match(drawing, /assets\/clipture-logo\.png/, 'Keep the approved original recorder render');
assert.doesNotMatch(drawing, /DrawString|Draw-Recorder/, 'Brand text must stay native, and the recorder must not be redrawn');
assert.match(theme, /NSD_CreateLabel.*CLIPTURE/, 'Sidebar branding must be native text');
const fitting = fs.readFileSync(path.join(root, 'src-tauri/installer/bitmap-fit.nsh'), 'utf8');
assert.match(fitting, /SetStretchBltMode\(p r2, i 4\)/, 'Use HALFTONE resampling');
assert.match(fitting, /GetClientRect/, 'Fit to real pixel dimensions, not assumed dialog-unit proportions');
for (const [name, width, height] of [['sidebarImage', 656, 1256], ['headerImage', 600, 228]]) {
  const bmp = fs.readFileSync(path.resolve(root, 'src-tauri', nsis[name]));
  assert.equal(bmp.toString('ascii', 0, 2), 'BM');
  assert.equal(bmp.readInt32LE(18), width);
  assert.equal(bmp.readInt32LE(22), height);
  assert.equal(bmp.readUInt16LE(28), 24, 'NSIS artwork uses opaque 24-bit BMP');
}
console.log('Pinned template: seven expected hooks; themed assets valid; startup checked, updates preserve choice.');
