# Installer compatibility seam

`tauri-2.11.4.nsi` is the upstream Tauri CLI 2.11.4 NSIS template (MIT OR
Apache-2.0), with seven product macro calls: start/end of `.onInit`, the startup
options page, finish-page setup/action, and installer/uninstaller progress styling.
The large file is vendored packaging infrastructure, not an application module.
Keep product behavior in the small `hooks.nsh` file; do not fork unrelated UI,
WebView2 prerequisite, shortcut or uninstall logic in the template.

## Visual theme

`theme.nsh` owns the silver/graphite field-recorder palette, typography, welcome
copy and orange native progress bar. `artwork/` contains 4x-resolution 24-bit BMPs:
a graphite welcome/finish sidebar and a silver header. The original
recorder render is preserved, not redrawn; the surrounding ruler and accents are
drawn in code. No additional text is baked into the artwork. Sidebar branding
and instructions use native Segoe UI labels. The application's existing
Windows icon is unchanged by this installer-artwork revision. Standard
Windows navigation, checkboxes, install-location and scope controls are retained.
No custom web runtime, skinning plugin, timers, or install-policy changes are used.

Regenerate artwork after editing the surrounding design or logo:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/release/build-installer-artwork.ps1
```

The styling uses [NSIS Modern UI's supported configuration and page callbacks](https://nsis.sourceforge.io/Docs/Modern%20UI%202/Readme.html).
Progress styling disables Windows visual styles only for the progress control,
so the brand orange is not replaced by the system's green progress effect.

`bitmap-fit.nsh` narrowly replaces MUI's two bitmap-loading macros. It measures
the actual image control, fits the artwork uniformly within both dimensions,
centers it on a matching background and uses GDI HALFTONE resampling. This avoids
FitControl's nonuniform stretching when fonts/DPI change the dialog-unit aspect
ratio. MUI frees each page bitmap; GUI teardown frees the header bitmap.
The graphics-only test exercises real GDI output on hidden scratch controls:

```powershell
node scripts/release/test-installer-bitmap-fit.cjs
```

This test never runs the real installer, touches the registry or launches Clipture.

Why a template is necessary: NSIS removes `/D` from its command-line buffer
before script execution, and MultiUser initialization overwrites `$INSTDIR`.
A pre-install hook is too late to recover that initial directory. The begin
hook saves it; the end hook restores it after MultiUser initialization.

For Electron updater invocations, the hooks inherit the registered legacy scope
and custom path when no scope or directory override is supplied. A read-only
`--verify-legacy-upgrade` command rejects known user data inside the old install
directory before invoking its uninstaller with `/KEEP_APP_DATA`. Installation
stops if preflight or legacy uninstall fails. `--force-run` is translated into
a post-install user launch. Keep this behavior in hooks, not the vendor template.

The interactive startup page defaults to checked. Finish applies `startOnLogin`
and the matching HKCU startup registration through an unelevated installer-only
command. That command does not create a host or engine, and starts the normal app
only if the user also checked Run Clipture. Existing settings are preserved;
registration failures are reported and the settings change is rolled back.
Silent, passive, and updater-mode installations do not override the preference.
Startup applies to the signed-in user even with an all-users installation.

After synchronizing the current startup preference, the installer helper and
normal Tauri host remove the known Electron Run identities
`electron.app.Clipture` and `app.clipture.desktop` only when their commands point
to Clipture. Originals are preserved under
`HKCU\Software\Clipture\Migration\LegacyStartup` before removal. The canonical
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Clipture` value is retained.
This does not uninstall Electron, close processes, or modify clip data.

After files and shortcuts are installed, `icon-refresh.nsh` sends targeted
`SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT)` notifications
for the executable and existing shortcuts whose target matches that installation.
This runs once per install/update (including silent updates), not at app startup.
It includes the installing user's existing taskbar pins and leaves their targets,
arguments, icon overrides and pin order unchanged. It never clears the global
icon cache or restarts Explorer. Windows controls repaint timing; pins owned by
another Windows account or pointing to an old installation are not modified.

Fast checks (no installation or recording):

```powershell
node scripts/release/test-installer-template.cjs
node scripts/release/test-installer-startup-page.cjs
cargo test --manifest-path src-tauri/Cargo.toml installer_startup --lib
```

When updating Tauri CLI, rebase these seven macro calls onto its new template and
repeat clean/custom-path, Electron cross-grade, silent relaunch and uninstall
tests in a disposable Windows VM. Do not test installers against the daily app.
