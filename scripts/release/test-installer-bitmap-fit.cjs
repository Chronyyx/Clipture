// Silent graphics-only test: hidden scratch controls, no installer or app actions.
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '../..');
const work = fs.mkdtempSync(path.join(root, '.cache/installer-art-fit-'));
const nsis = path.join(process.env.LOCALAPPDATA, 'tauri/NSIS/makensis.exe');
// Deterministic color target isolates fitting from texture/antialiasing in the
// approved render. Same source dimensions as the real installer artwork.
function probeBitmap(w, h, name, background) {
  const stride = (w * 3 + 3) & ~3;
  const bmp = Buffer.alloc(54 + stride * h);
  bmp.write('BM'); bmp.writeUInt32LE(bmp.length, 2); bmp.writeUInt32LE(54, 10);
  bmp.writeUInt32LE(40, 14); bmp.writeInt32LE(w, 18); bmp.writeInt32LE(h, 22);
  bmp.writeUInt16LE(1, 26); bmp.writeUInt16LE(24, 28);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const color = Math.abs(x - w / 4) < w / 16 && Math.abs(y - h / 4) < h / 16 ? 0xff6500 : background;
    const i = 54 + (h - 1 - y) * stride + x * 3;
    bmp[i] = color & 255; bmp[i+1] = (color >> 8) & 255; bmp[i+2] = color >> 16;
  }
  const output = path.join(work, name + '.bmp');
  fs.writeFileSync(output, bmp);
  return output;
}
const sidebarProbe = probeBitmap(656, 1256, 'sidebar-probe', 0x1b1c19);
const headerProbe = probeBitmap(600, 228, 'header-probe', 0xf0f0eb);
const sizes = [[164, 400], [205, 500], [246, 600], [328, 800], [260, 250], [150, 76], [225, 114], [300, 152]];
const cases = sizes.map(([w, h], index) => {
  const header = index >= 5;
  const [sw, sh] = header ? [600, 228] : [656, 1256];
  const [dotX, dotY] = [sw / 4, sh / 4];
  const background = header ? '0xEBF0F0' : '0x191C1B';
  const scale = Math.min(w / sw, h / sh);
  const x = Math.floor(Math.floor((w - Math.floor(sw * scale)) / 2) + dotX * scale);
  const y = Math.floor(Math.floor((h - Math.floor(sh * scale)) / 2) + dotY * scale);
  return `
  System::Call 'user32::CreateWindowExW(i 0, w "STATIC", w "", i 0x8000000E, i 0, i 0, i ${w}, i ${h}, p 0, p 0, p 0, p 0) p.r0'
  FileWrite $R8 "case ${index}: window=$0$\\r$\\n"
  StrCmp $0 0 fail${index}
  !insertmacro CLIPTURE_FIT_BITMAP $0 "${header ? headerProbe : sidebarProbe}" ${background} $1
  FileWrite $R8 "bitmap=$1$\\r$\\n"
  StrCmp $1 0 fail${index}
  System::Call 'gdi32::GetObjectW(p r1, i 32, @r2)'
  System::Call '*$2(i, i.r3, i.r4)'
  FileWrite $R8 "size=$3,$4$\\r$\\n"
  IntCmp $3 ${w} +2
  Goto fail${index}
  IntCmp $4 ${h} +2
  Goto fail${index}
  System::Call 'gdi32::CreateCompatibleDC(p 0) p.r2'
  System::Call 'gdi32::SelectObject(p r2, p r1) p.r5'
  System::Call 'gdi32::GetPixel(p r2, i 0, i 0) i.r3'
  FileWrite $R8 "background=$3$\\r$\\n"
  IntCmp $3 ${background} +2
  Goto fail${index}
  ; Known color target, transformed with uniform scale.
  System::Call 'gdi32::GetPixel(p r2, i ${x}, i ${y}) i.r3'
  FileWrite $R8 "dot=$3$\\r$\\n"
  IntCmp $3 0x0065FF +2
  Goto fail${index}
  System::Call 'gdi32::SelectObject(p r2, p r5)'
  System::Call 'gdi32::DeleteDC(p r2)'
  System::Call 'user32::DestroyWindow(p r0)'
  System::Call 'gdi32::DeleteObject(p r1)'
  Goto done${index}
fail${index}:
  FileClose $R8
  SetErrorLevel ${index + 1}
  Quit
done${index}:
`;
}).join('\n');
const source = `Unicode true
!include MUI2.nsh
!include LogicLib.nsh
!include "${path.join(root, 'src-tauri/installer/bitmap-fit.nsh')}"
Name "Clipture graphics self-test"
OutFile "${path.join(work, 'graphics-test.exe')}"
RequestExecutionLevel user
SilentInstall silent
Section
  FileOpen $R8 "${path.join(work, 'graphics-test.log')}" w
${cases}
  FileClose $R8
  SetErrorLevel 0
SectionEnd
`;
fs.writeFileSync(path.join(work, 'graphics-test.nsi'), source);
const compile = spawnSync(nsis, ['/V2', path.join(work, 'graphics-test.nsi')], { encoding: 'utf8', windowsHide: true });
if (compile.error) throw compile.error;
if (compile.status !== 0) throw new Error(compile.stdout + compile.stderr);
if (process.argv.includes('--compile-only')) {
  console.log('Compiled graphics-only test: ' + work);
} else {
  const run = spawnSync(path.join(work, 'graphics-test.exe'), ['/S'], { windowsHide: true, timeout: 15000 });
  if (run.error) throw run.error;
  if (run.status !== 0) throw new Error(`Native bitmap fitting failed case ${run.status}: ${JSON.stringify(sizes[run.status - 1])}\n${fs.readFileSync(path.join(work, 'graphics-test.log'), 'utf8')}`);
  console.log('Native bitmap fit passed: sidebar at 100/125/150/200%, short/wide panels, and three header sizes; actual GDI pixels verified.');
}
