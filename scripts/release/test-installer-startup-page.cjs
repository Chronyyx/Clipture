// Compile a tiny inert NSIS harness; never install or launch Clipture.
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '../..');
const cache = path.join(root, '.cache');
fs.mkdirSync(cache, { recursive: true });
const work = fs.mkdtempSync(path.join(cache, 'installer-startup-'));
const nsis = path.join(process.env.LOCALAPPDATA, 'tauri', 'NSIS');
const branding = require('../../src-tauri/tauri.conf.json').bundle.windows.nsis;
const asset = key => path.resolve(root, 'src-tauri', branding[key]);
const source = `Unicode true
!include MUI2.nsh
!include FileFunc.nsh
!include LogicLib.nsh
!include nsDialogs.nsh
!include "Win\\COM.nsh"
!include "${path.join(root, 'src-tauri', 'target', 'release', 'nsis', 'x64', 'utils.nsh')}"
!addplugindir "${path.join(nsis, 'Plugins', 'x86-unicode', 'additional')}"
Name "Clipture inert installer test"
OutFile "${path.join(work, 'compile-test.exe')}"
RequestExecutionLevel user
SetCompress off
!define MAINBINARYNAME "__clipture_inert_no_application__"
!define PRODUCTNAME "Clipture inert installer test"
Var PassiveMode
Var UpdateMode
Var AppStartMenuFolder
!include "${path.join(root, 'src-tauri', 'installer', 'hooks.nsh')}"
!define MUI_ICON "${asset('installerIcon')}"
!define MUI_UNICON "${asset('uninstallerIcon')}"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "${asset('headerImage')}"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${asset('sidebarImage')}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro CLIPTURE_STARTUP_PAGE
!insertmacro CLIPTURE_PROGRESS_STYLE ""
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION RunMainBinary
!insertmacro CLIPTURE_FINISH_SETUP
!insertmacro MUI_PAGE_FINISH
!insertmacro CLIPTURE_FINISH_ACTION
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro CLIPTURE_PROGRESS_STYLE "un."
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
Function RunMainBinary
FunctionEnd
Section
!insertmacro NSIS_HOOK_POSTINSTALL
SectionEnd
Section "Uninstall"
SectionEnd
`;
const script = path.join(work, 'compile-test.nsi');
fs.writeFileSync(script, source);
const result = spawnSync(path.join(nsis, 'makensis.exe'), ['/V3', script], { encoding: 'utf8', windowsHide: true });
process.stdout.write(result.stdout || '');
process.stderr.write(result.stderr || '');
if (result.error) throw result.error;
if (result.status !== 0 || /unknown (variable|constant)|invalid command/i.test(result.stdout + result.stderr)) {
  process.exitCode = 1;
} else {
  console.log('Themed welcome/directory/startup/progress/finish and uninstall pages compiled. Harness was not executed: ' + work);
}
