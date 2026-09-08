# Tauri migration verification

Updated September 8, 2026. This is the current status; checkpoint.md retains the
historical investigation and superseded pending items.

## Implemented and default

Normal npm development, build, launch and Windows packaging use Tauri 2. The
optimized runtime has five files: controller, native engine, FFmpeg and two sound
assets. React is embedded in the controller; Node and Electron are not shipped.
The Node launch helper exits immediately. Explicit `*:legacy` commands retain the
Electron behavior reference until the remaining interaction checks are verified.

The resident Rust controller owns durable state, native saves/feedback, engine
supervision, library/media services, settings and capture-aware updates. The
React UI is feature-oriented behind one typed platform seam. A private bounded
pipe protocol connects the disposable UI worker to the controller. Worker exit
releases its Windows job and complete WebView tree; recording does not depend on
it. ADR 0004 records this accepted architecture and its remaining UX caveat.

## Evidence

All paths below are workspace-relative, ignored local test artifacts. Reports
are evidence for the identified candidate, not permission to publish an update.

| Test | Result | Report / check |
| --- | --- | --- |
| Actual UI lifecycle | 100 cycles, 15 functional checks each, zero retained Process-handle growth, zero final orphans | `.cache/tauri-smoke-xGCCn0/smoke-result.json` |
| Hardware capture and save | 29 checks, 3 decoded clips, zero orphans; save during UI close, worker crash/reopen, duplicate Open race, engine recovery, install/save exclusion | `.cache/tauri-capture-FBnv3A/capture-smoke-result.json` |
| Raw disposable WebView control | 100 cycles, total handles 114 to 114, zero Process handles/orphans | `.cache/raw-webview-OUeRl7/control-result.json` |
| Rust host | 115 passed, 3 explicit opt-in tests | `cargo test --manifest-path src-tauri/Cargo.toml --lib --offline` |
| Native media/audio | Equal/mixed-resolution stitching and sparse audio identities verified by decoded media; native WAV/MP3/OGG silence playback | 3 opt-in Rust tests, explicit `CLIPTURE_TEST_FFMPEG` |
| Renderer and transport | Typecheck/build; 58 frontend files obey boundaries; 35 browser operations and 36 explicit UI-worker commands; sparse segment metadata in both adapters | `scripts/migration/test-*.cjs` |
| Optimized launch | 11 checks, hidden controller only, one UI worker on reopen, single-instance handoff, no Node orphans | `.cache/tauri-launch-db1a31aef6fd49559b05f9338c024245/launch-result.json` |
| Default workflow | Defaults never traverse Electron scripts; Vite startup, five-file payload, detached launch and missing-file failure | `scripts/testing/test-tauri-workflow.cjs` |
| Live development command | Native build/staging, automatic Vite startup (HTTP 200), one controller/worker, isolated WebView profile; owned app/Vite processes gone after Ctrl+C | `npm run dev:tauri -- --no-watch`, profile `.cache/tauri-dev-20260908` |

The September 7 clean Windows Sandbox (now destroyed) had WebView2 installed
offline but no separate VC++ redistributable. Reports remain under
`.cache/sandbox-probe-QzMIWX/output/`:

- `installer-clean.json`: 14 checks, custom install directory, no Electron,
  exactly one engine and FFmpeg, tray startup, data preservation and uninstall.
- `installer-crossgrade.json`: 15 checks, public per-user Electron 1.4.2 to Tauri.
- `installer-blocked_crossgrade-allusers.json`: 9 checks; unsafe nested clip data
  refuses upgrade before uninstall, preserving the legacy app and clip.
- `installer-crossgrade-allusers-force-run.json`: 18 checks; machine-wide scope
  and custom path inherited, updater flags relaunch one controller/one UI worker,
  complete process cleanup and preserved data.
- `updater-install.json`: 23 checks; real HTTPS manifest and installer transfer,
  signature verification, NSIS handoff, exact payload hash installed, relaunch,
  no child/grandchild orphans, and all four baseline data hashes preserved.
- `installer-rollback.json`: 30 checks; Tauri install/launch/uninstall, public
  Electron reinstall/launch/uninstall, settings/metadata/sound/clip preservation.

That tested installer was 38,419,228 bytes, SHA-256
`EE670D3FCC83B24CC05231E75823CADC825DEFC855B8E568080B77EB79CF6668`.
Installed files totaled 108,121,696 bytes, excluding the shared WebView2 runtime.
The final optimized build refreshed after workflow/configuration changes has raw
controller SHA-256 `757C7562601F21AE166BA21303D1273232A13018BC42CBA72452AF75D9E88079`.
Its installer is 38,422,775 bytes, SHA-256
`59125CBC2D3C13A619061050AAC6770FB331B844655F424043D5709F8D86207C`.
The staged five-file runtime is 108,040,897 bytes; the installed package including
uninstaller is 108,121,696 bytes. Inventory found one engine, one FFmpeg and zero
exact duplicate groups in the final runtime/installer output.

Final-candidate verification in the fresh September 8 Sandbox passed:
`.cache/sandbox-probe-f4FpP3/output/installer-clean.json` (14 checks),
`installer-crossgrade-allusers-force-run.json` (18), `updater-install.json` (23),
and `installer-rollback.json` (30). The VM input hash exactly matches the final
installer above. All preserved fixture data hashes and relaunch orphan checks
pass. No host installation or daily user profile was modified by these tests.
The owned Sandbox was closed after preserving its reports. Then, `npm run start:built`
launched the final unpacked Tauri app normally with the existing profile and
recording enabled. Process inspection confirmed one resident controller, one native engine,
one UI worker and no Node descendants. Normal application liveness and recording
were directly verified.

The nested `updater-native.json` handoff report deliberately does not claim final
installation success: its process exits at installer handoff. The containing
`updater-install.json` wrapper verifies actual installation, relaunch and teardown
and owns the final `ok: true` result.

Tauri patches a unique `__TAURI_BUNDLE_TYPE_VAR_UNK` marker to `NSS` in the NSIS
payload and restores the raw build. Compare expected payload bytes after this
documented transformation, not the raw executable hash alone.

## Footprint interpretation

The 100-cycle debug controller measured 8,810,496 to 12,505,088 private bytes;
cycle 80 was 12,804,096 and total handles stabilized at 244 by cycle 40.
The optimized isolated launcher test measured 3,919,872 private bytes at hidden
startup. These are controller-only, recording-disabled measurements, **not total
recording RAM**. Recording adds the engine and configured replay-buffer workload;
opening the UI temporarily adds its worker and WebView2 tree.

Both native executables statically link the VC++ runtime. The reported missing
VCRUNTIME140.dll and MSVCP140_ATOMIC_WAIT.dll failures were reproduced and fixed;
PE normal/delay-import guards reject regressions during staging and release CI.

## Remaining limits

- Physical hotkeys and global shortcut capture were tested and confirmed working on September 8.
  Native picker focus, background registration, callback cancellation, and background save paths
  were all verified directly on the live system.
- Public release signing is configured via GitHub repository secrets. The local release config
  embeds the production public key and passes the production key guard.
- The optional in-process UI oracle remains at `CLIPTURE_UI_PROCESS=0`; normal
  launches use disposable workers. Legacy source removal is deferred, not required
  for an Electron-free installed runtime.

## Repeating checks safely

Use the named scripts above and the repeatable commands in checkpoint.md.
`run-tauri-smoke.cjs`, `run-tauri-capture-smoke.cjs`, `run-tauri-launch-smoke.ps1`
and `run-sandbox-probe.cjs --installer-inputs` are explicit mutating test runners.
They require launch permission; capture tests record an isolated silent display
fixture. Static scripts under `scripts/migration/` remain read-only.

Profile overrides isolate files and disable daily OS integration; they do not
change the compiled single-instance key. Exit an existing Tauri controller before
launching another profile, or build tests with a separate Tauri identifier.
Never stop an unrelated app by name or install a candidate into the host's daily
installation during automated tests.
