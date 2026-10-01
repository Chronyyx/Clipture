# Tauri release and Electron cross-grade

Clipture's Windows release is one x64 NSIS installer with two update manifests:

| Asset | Consumer | Trust check |
| --- | --- | --- |
| Tauri NSIS `.exe` | New installs, Electron cross-grade, Tauri updater | HTTPS plus updater signature for Tauri clients; SHA-512 for Electron clients |
| Matching `.exe.sig` | Tauri updater | Minisign public key embedded in the app |
| `latest.json` | Tauri 2 clients | Contains the exact NSIS URL and signature text |
| `latest.yml` | Existing Electron clients | Contains the exact same NSIS URL, size, and SHA-512 |

The release workflow creates a draft first, validates all four assets, and only then marks it latest. A failed bridge or signature check leaves a draft instead of publishing a release that strands one client generation.

## Required GitHub secrets

- `TAURI_SIGNING_PRIVATE_KEY`: the complete Tauri updater private key or a secure path available only on the runner.
- `TAURI_SIGNING_PRIVATE_KEY_PASSWORD`: the non-empty password for that key.
- `GITHUB_TOKEN` is supplied automatically by Actions and receives `contents: write` only in the release workflow.

The updater private key is not Windows Authenticode signing. Never commit, print, upload, or put the updater private key in a `.env` file. The developer key currently held outside this repository must remain outside this repository and must not be read by tests or migration scripts.

Release CI rejects malformed public keys and the known local development key via
`scripts/release/validate-production-key.cjs`. The current configured key passes
that read-only guard; this does not prove that CI has its matching private key.
Production signing requires that match. Tauri requires key content, not a
filesystem path. After clients ship, rotation needs a staged plan and an ADR.

## Bundled FFmpeg

`npm run stage:tauri` stages the FFmpeg pinned in `scripts/fetch-ffmpeg.cjs`
(gyan.dev essentials build, currently 9.0.2). The script downloads it once into
`build/ffmpeg/<version>/`, checks the archive and the extracted `ffmpeg.exe`
against pinned SHA-256 hashes, and fails on any mismatch; release CI re-checks
the staged binary with `scripts/release/test-ffmpeg-pin.cjs`.
`CLIPTURE_FFMPEG_PATH` still overrides the source for local experiments. To
update, change the version, URL and both hashes together (verify the archive
hash against gyan.dev's published `.sha256`), then run the real-FFmpeg tests:
`CLIPTURE_TEST_FFMPEG=<path> cargo test -- --ignored`. The npm
`ffmpeg-static` package remains only for the legacy Electron build and
developer scripts.

## Windows code signing (Authenticode)

Signing is opt-in and off until configured; unsigned releases log a warning.
It is separate from the updater key above: Authenticode lets Windows and
SmartScreen verify the publisher of the installer and every executable.

- Repository variable `WINDOWS_SIGN_COMMAND`: the command that signs one file,
  with `%1` where the file path goes. Tauri runs it for the app and installer,
  and the workflow runs it for the three standalone runtime binaries before
  their manifest hashes are computed.
- Repository variable `WINDOWS_SIGN_INSTALL` (optional): installs the signing
  tool on the runner.
- Secrets `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET`: exposed
  only to the build and runtime-publish steps.

For Azure Artifact Signing (formerly Trusted Signing), following Tauri's
documented invocation (confirm flags with the tool's `--help`):

```text
WINDOWS_SIGN_INSTALL = cargo install artifact-signing-cli --version 0.11.0 --locked
WINDOWS_SIGN_COMMAND = artifact-signing-cli -e https://<region>.codesigning.azure.net -a <account> -c <certificate-profile> -d Clipture %1
```

When signing is configured, the release fails unless the installer and the
runtime binaries all carry a valid Authenticode signature.

Workflow actions are pinned to commit SHAs; `.github/dependabot.yml` proposes
updates. Workflow scripts read the tag, titles and asset names from the
environment rather than from expressions spliced into script text.

## Host integration status

The implementation under `src-tauri/src/updates/` is connected to the host:

1. The updater dependency is registered in `src-tauri/Cargo.toml`:

   ```toml
   [target.'cfg(any(target_os = "macos", windows, target_os = "linux"))'.dependencies]
   tauri-plugin-updater = "2.11.0"
   ```

2. `src-tauri/src/lib.rs` declares the module and registers the updater plugin.
3. App setup manages the service with a narrow capture/save gate. Isolated test
   profiles disable updates and startup registration.
4. These thin commands are registered in `tauri::generate_handler!`:

   ```rust
   updates::commands::get_update_state,
   updates::commands::check_for_updates,
   updates::commands::download_update,
   updates::commands::install_update,
   ```

5. Host capabilities advertise the commands and `updates://state-changed`.

The service publishes the renderer's existing state shape without a WebView.
The native controller checks once, four seconds after startup, and automatically
stages a signed runtime from the GitHub release. Reopening the UI does not check
again. The runtime manifest and its Minisign signature authorize exactly five
files, their sizes, SHA-256 digests and 1 MiB block maps. Manual checks remain
retryable; the once-only policy belongs to the background scheduler, not the
service. Unchanged whole files and matching blocks (including shifted blocks)
are reused. Missing blocks use validated HTTP ranges; a server returning a full
200 response falls back to a bounded, authenticated whole-file download. Release publishing
signs and verifies `components-v1.json` with the existing updater key.

Runtime files live under `%LOCALAPPDATA%\Clipture\runtime`; durable data and
`saveFolder` remain unchanged. Activation occurs on the next full launch or by
explicit Apply now confirmation. Native activation restarts the controller and
engine and loses unsaved replay. Live engine-only replacement is not implemented.
Compatible UI workers can use the staged executable without restarting capture;
compatibility defaults to full restart when the compiled native identity differs.
The original installed entrypoint forwards to verified newer runtimes without
an installer or elevation. Previous versions are retained. The handoff supervisor
waits for both the Tauri event loop and successful engine configuration/hotkey
IPC with `engineRunning`, then observes five seconds of controller liveness.
A window appearing alone is not a successful update. Failed startup kills the
owned candidate tree and restarts the previous controller. The exact failed
signed runtime is excluded from staging, activation and compatible UI refresh;
later releases remain eligible. Failure to write the quarantine marker must not
prevent attempting recovery. This is startup health, not capture-quality proof.
Real signed process-handoff validation in a disposable Windows account/VM remains a release gate.
Do not treat unit tests as evidence that activation succeeded in a real build.

See [ADR 0009](../adr/0009-signed-component-runtime-updates.md) for the trust and
activation boundaries and the remaining release checklist.

The legacy NSIS path remains for older clients and the isolated installer smoke:
New windows request the current snapshot. Downloads stream into temporary files
with incremental Minisign verification, a 512 MiB payload cap and 512 KiB writes.
Healthy/elevated/critical capture pressure sets transfer ceilings of 32/16/4 MiB/s;
unknown pressure and active saves pause transfer. Continuous pauses time out
after 30 minutes. Modern prehashed signatures are required; legacy unprehashed
signatures fail closed. No installer-sized buffer stays resident while ready.
Installation re-reads and verifies the staged bytes, then reserves the same
atomic gate as saving. A plugin launch failure restarts capture after its
before-exit hook. The plugin still requires a transient full buffer at install.

Local transport/signature tests and simulated recovery pass. They do not replace
a signed production installer, actual cross-grade, rollback and uninstall test.

The Rust integration follows the official [Tauri 2 updater setup](https://v2.tauri.app/plugin/updater/). The release job uses the official [Tauri GitHub Action](https://github.com/tauri-apps/tauri-action), enables `createUpdaterArtifacts`, explicitly prefers NSIS in `latest.json`, and supplies signing variables through the job environment.

## Bridge-release sequence

1. Keep the last Electron build available as the behavior and rollback reference. Its updater must still use the GitHub `latest.yml` channel.
2. Release the first Tauri build at a strictly higher semantic version. Do not reuse an Electron version.
3. The workflow signs the exact Tauri NSIS installer and publishes both `latest.json` and the generated `latest.yml`. Electron downloads the full Tauri installer; no Electron blockmap is promised for this cross-grade.
4. Test an upgrade in a clean Windows VM from the latest public Electron installer. Verify settings, `%APPDATA%\Clipture\data`, the configured `saveFolder`, shortcuts, startup registration, tray startup, capture, save, and uninstall behavior.
5. Keep emitting `latest.yml` on every release while unsupported Electron installations may still contact `/releases/latest/download/latest.yml`. A user who skips the first Tauri release otherwise cannot cross-grade after a newer release becomes latest.
6. Retire `latest.yml` only after an explicit support-window decision. Removing Electron source code is independent from keeping this small compatibility manifest.

Do not point Electron at a wrapper, bootstrapper, MSI, or differently hashed copy. `latest.yml` must name and hash the same NSIS `.exe` uploaded for the Tauri updater. The workflow enforces exactly one x64 NSIS installer and verifies its URL against `latest.json` before publishing.

## Local and CI checks

Windows executables must not depend on a separately installed Visual C++ runtime.
The C++ engine uses CMake's static MSVC runtime; direct Cargo builds enable the
same static VC runtime linkage as the Tauri CLI. Sidecar staging rejects VC
runtime DLL imports, including delay imports. CI checks the final controller
and both sidecars before publishing. Run the read-only regression with
`node scripts/release/test-windows-runtime-imports.cjs`. A clean-VM engine
startup/diagnostics/EOF-shutdown test also verifies the result without capture.

The metadata generator is deterministic when version, installer bytes, asset name, and release date are fixed:

```powershell
node scripts/release/test-electron-bridge.cjs
```

For a release candidate, run the normal renderer, engine, host-contract, and Rust tests before building. `scripts/release/create-electron-bridge.cjs` accepts `--signature` and `--latest-json`; release CI supplies both, so an unsigned or mismatched Tauri manifest cannot produce bridge metadata.

Never test updater installation against a developer's daily profile. Use a disposable VM and an isolated test account. The release test is destructive by nature: it closes the old application, runs an installer, and changes installed-program state.
