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

The updater private key is not Windows Authenticode signing. Add certificate-based Windows code signing separately before a public production launch if SmartScreen reputation is required. Never commit, print, upload, or put the updater private key in a `.env` file. The developer key currently held outside this repository must remain outside this repository and must not be read by tests or migration scripts.

The public verification key currently committed in `src-tauri/tauri.release.conf.json` belongs to a LOCAL DEVELOPMENT TEST key. It is not a production signing setup and must not be published as one. Before the first public Tauri release, configure the production public key and matching CI secrets. Tauri requires key content, not a filesystem path. After clients ship, rotation needs a staged plan and an ADR.

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
