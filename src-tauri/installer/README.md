# Installer compatibility seam

`tauri-2.11.4.nsi` is the upstream Tauri CLI 2.11.4 NSIS template (MIT OR
Apache-2.0), with two macro calls added at the start/end of `.onInit`.
The large file is vendored packaging infrastructure, not an application module.
Keep product behavior in the small `hooks.nsh` file; do not fork unrelated UI,
WebView2 prerequisite, shortcut or uninstall logic in the template.

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

When updating Tauri CLI, rebase these two macro calls onto its new template and
repeat clean/custom-path, Electron cross-grade, silent relaunch and uninstall
tests in a disposable Windows VM. Do not test installers against the daily app.
