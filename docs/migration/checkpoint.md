# Migration checkpoint — 2026-09-05

Status: Tauri is the default build/development/launch/package path. Core migration
and automated integration gates pass; native keyboard and picker UX have been verified.
See [current verification](verification.md) for the authoritative status and exact
artifact evidence. Public release signing is configured via repository secrets.
The September 7 Sandbox was destroyed during host PC shutdown; never reuse its ID.
Tests must continue using isolated profiles, never the daily library or settings.

## Historical investigation log

The entries below preserve investigation history. Their pending/default/guest
statements are superseded by verification.md, not instructions to repeat old work.

- September 7 follow-up: actual disposable app stress xGCCn0 PASS 100 cycles,
  all 15 per-cycle checks including a 24-request IPC burst, zero WebViews after
  every close, zero retained Process-handle growth, zero final orphans.
  Controller private bytes 8,810,496 to 12,505,088; sampled cycle 80 was
  12,804,096 (not monotonically growing). Total handles 235 to 244, stable from
  cycle 40. These are debug/test-mode controller numbers, not recording RAM.
- Preceding actual stress ZydzVY stopped at cycle 88: functional checks passed,
  but two browser children outlived the close limit. A private, non-inheritable
  Windows job now owns each UI worker and descendants, assigned before bootstrap.
  It never contains the recorder/controller. Ownership regression proves job
  closure leaves a separately spawned sibling untouched. The fixed run is above.
- Latest hardware capture FBnv3A PASS 29 checks, three decoded clips, zero orphans.
  UI crash and duplicate Open requests during close preserve the recording engine
  generation and produce exactly one replacement UI. Saves survive UI closure.
- UI backpressure now preserves the latest of eight state hints, coalesces
  lifecycle signals and reliably returns overload errors. Broken-writer,
  saturation, stale reply, timeout and replay-window regressions pass. Duplicate
  close creates one watchdog. FFmpeg wait-error cleanup now reaps its child and
  reader threads. Native picker callbacks replace blocking async-thread waits;
  their GUI behavior is still unverified.
- Tauri candidate now defaults to disposable UI hosting; CLIPTURE_UI_PROCESS=0
  keeps the old in-process migration oracle. Electron remains the repository
  default until final packaging gates. Latest optimized installer is building.
- Corrected raw memory run OUeRl7 PASS 100: zero Process handles, total handles
  114 to 114, private bytes 4,075,520 to 4,321,280, zero orphans. Older notes
  below saying this run or RuDboE capture is pending are historical.
- Restaged native engine SHA256:
  1cbb828d99967d21ea2f29fd60b0e2dd2dd1ca2407d771d26d8bd70fcbb7de76.
  Host/renderer boundary and PE runtime-import tests pass.
- Physical hotkeys and native dialog UX have been verified directly on the host system.
- Fresh guest: 66ce9501-c6d4-41f6-8cf5-ea175de88b7e, profile
  .cache/sandbox-probe-QzMIWX (token in input/probe-token.txt). Offline WebView2
  installation passed. The previous guest no longer exists. Do not run installer
  phases until its Tauri.exe input is refreshed from the newly rebuilt candidate.

- Current disposable-process integration: CLIPTURE_UI_PROCESS=1, opt-in only.
  ADR 0004 is proposed until app/installer gates pass. The resident controller
  owns all durable services; a --ui-worker role branches before single-instance
  setup and receives only a private stdio bootstrap. No loopback listener.
  UI RPC dispatch matches all 36 existing registered commands; media bodies
  are binary, capped at 4 MiB; JSON frames are capped at 16 MiB. Pending requests
  are capped at 24 and release on cancellation, timeout, or EOF. Six requests
  currently run concurrently in the resident dispatcher; saturation/event
  backpressure and Open-during-close races still need hardening.
- Raw disposable-process test PASS 100/100, zero Process handles at every close,
  unchanged total handle count (114), zero browser children/orphans:
  .cache/raw-webview-NEzmdZ/control-result.json. Its private bytes grew 4.08 to
  7.48 MB while the test retained all JSON snapshots; do not attribute that
  report storage to the runtime. The harness now writes per-cycle reports and
  aggregates only after final measurement. Repeat raw-webview-OUeRl7 is running.
- Actual disposable Tauri UI PASS 3 cycles and all 14 per-cycle functional checks:
  .cache/tauri-smoke-Li2HrM/smoke-result.json. React, real private-pipe IPC, settings,
  library, byte ranges, video decode, audio mix, thumbnails, scoped sound assets,
  path rejection and offline test-mode guards pass. Closed controller has zero
  Process handles; private bytes approximately 8.74 to 9.11 MB, no child orphans.
  Explicit worker controller.Close teardown was added after this run to address
  Chrome_WidgetWin_0 teardown warnings; the new debug app build is in progress.
- Capture smoke now has opt-in worker-crash/reopen checks; not run yet with the
  new worker role. Native picker operations remain controller-owned; in detached
  mode their dialogs are unparented (never trust a renderer-supplied HWND).
  Actual modal/focus UX and physical hotkey delivery still need verification.
- Latest Rust suite: 110 passed, 3 opt-in tests; all three opt-in tests have also
  passed separately. New wire tests caught serde accepting unknown fields for
  unit enum variants; empty struct variants fix it, and regression now passes.
  Legacy 90-test counts below are historical checkpoints, not latest totals.
- Sparse segment audio is now implemented and verified with decoded tones:
  continuous 440 Hz microphone, app silence then 880 Hz, reversed input stream
  order corrected. MuxResult now reports actual logical stream identities and
  ClipRecord adds optional segmentAudioTracks. Shared fixture, both adapters,
  Rust round trip, TypeScript typecheck and native mux CTest pass. The old
  Electron stitcher refuses changing audio layouts instead of corrupting them.
  The complete migration contract and artifact inventory checks were rerun.

- Fixed a real save-parity omission: the engine's segmented result points to a
  destination that does not exist until the host stitches it. Tauri now runs a
  dedicated SavedClipProcessor off the UI/async thread before library commit.
  Media owns segment normalization/concatenation, sizing and system-audio mix;
  clips owns category publication. Failed processing preserves engine inputs.
  Publication never overwrites an existing clip. NTFS uses a hard link; other
  filesystems use a staged, no-clobber copy. Only successful publication removes
  the exact source inputs. Temporary processing files are scoped to a TempDir.
- Synthetic equal-resolution stream-copy and mixed-resolution/audio saves PASS:
  complete output decodes, expected 320x180 frames, approximately two seconds,
  three correctly labeled output audio tracks, quoted filenames, software
  fallback and temporary cleanup. Explicit opt-in Rust test:
  segmented_saves_decode_with_expected_dimensions_audio_and_duration.
- Latest rebuilt app hardware test PASS 23 checks, three decoded clips, no
  remaining children: .cache/tauri-capture-DBp7GS/capture-smoke-result.json.
  This includes the new save processor, native sound implementation (sound is
  disabled in the capture fixture), and hotkey overflow recovery.
- Hotkey broadcast overflow no longer permanently terminates the save listener;
  focused regression passes. Actual key delivery remains unverified: Computer
  Use initialization failed to write its kernel assets, including after reset.
- Installer preflight now protects the actual segmentFiles contract field (and
  the old segments shape); the type-backed regression passes.
- Current save-processing limits: 32 segments, 16 audio tracks; sequential
  FFmpeg jobs, 180-second per-attempt timeout, two software encoder threads.
  Remaining review: sparse/mismatched audio layouts across segments and final
  integrated resource/error coverage. Do not claim all media parity from the
  synthetic cases above. Latest installer predates these changes.
- Long raw WebView settle experiment FAIL: ten retained Process handles remain
  after 660 seconds with no WebView children. Report:
  .cache/raw-webview-SEsRMJ/control-result.json. No forced DLL unload or manual
  closure of runtime-owned handles was used.

## Verified

- All 35 host operations are represented by the typed Electron/Tauri seam.
- Rust domains own settings, engine supervision, saves, library, media,
  native feedback, diagnostics and updates. UI close destroys the WebView.
- Renderer uses feature-root exports; the app composition is 148 lines.
  `test-renderer-boundaries.cjs` rejects host imports outside platform,
  cross-feature deep imports and feature cycles.
- Real embedded-WebView smoke tests cover media decoding, opaque sessions,
  range responses, multi-track mixing, thumbnails, settings and sounds.
- Real hardware capture smoke passed with an isolated, silent profile:
  `.cache/tauri-capture-TJZ99s/capture-smoke-result.json`. Three clips decoded;
  one saved without any UI and one finished after closing the UI mid-save.
  Native feedback did not create a WebView. Hotkey registration passed;
  automatic crash restart and recovery after a simulated installer-launch
  failure passed. An actual physical keypress remains a separate gate.
- Atomic install/save exclusion is implemented and tested with concurrent
  requests. Failed installer launch restores the native engine configuration.
- Updates stream to a temporary file with incremental signature verification,
  bounded writes, size limits, capture-pressure pacing and save-time pauses.
  Valid, tampered, truncated and oversized payload tests pass. Installation
  re-verifies staged bytes before handing them to the native updater.
- Native OS/memory and actual hotkey status replace placeholder diagnostics.
  Frame-drop analysis matches the Electron oracle: 16 freshness cases and
  seven recorder observations including resets and bounded history.
- Library startup-event races and stale update snapshots are fixed. A pure
  library model regression suite passes; the Rust engine client is split into
  control, supervision and core ownership modules.
- Latest Rust suite: 90 passing tests plus one opt-in native audio test (also passed separately), including installer data-safety preflight and tray-first bundled-sound
  resolution before any UI enumeration. Existing sound files are preserved.
- Latest hardware run passed 23 checks, including restored hotkey registration
  after crash and failed-update recovery, plus zero remaining child processes.
  Repeated with the statically linked engine: tauri-capture-0ok9GI, all three
  clips decode. This run predates the new background audio player integration.
- Native WAV, MP3 and OGG decode/play passed using generated 100 ms silence:
  sounds::native_tests::native_background_sound_formats_without_webview.
  The MCI player is replaced by one bounded, on-demand FFmpeg/PCM worker and
  a Win32 synchronous wave sink. No idle worker, browser or decoded-audio cache.
  Feedback PCM cap is 16 MiB (about 87 seconds at stereo 48 kHz), decoding times
  out after 15 seconds; overlapping feedback is rejected, not queued unboundedly.
  Decoder failures release the slot; unit regression passes. UI preview still
  uses browser audio and has not been changed. The integrated debug app rebuild
  is in progress; the tested installer predates this audio implementation.
- Local unsigned optimized NSIS build succeeded: 38,214,133 bytes, controller
  executable 8,509,952 bytes. This is a packaging candidate, not a signed release
  or installed-footprint measurement. Sidecars are staged under src-tauri/binaries.
- App-exit orphan checking passed in `.cache/tauri-smoke-EuOKGY`: no remaining
  children. This run still FAILED the retained-handle lifecycle gate.

## Known failing or unfinished gates

1. **Lifecycle leak:** 100 functional open/close cycles passed and all WebView2
   descendants exited, but controller Process handles grew from 1 to 100.
   Reproduces with blank HTML, without React or media. Explicit controller
   Close fixes invalid-HWND teardown warnings, not this leak. Environment reuse
   and disabling drag/drop did not fix it. WebView2 runtime: 152.0.4191.62.
   Corrected resource report: `.cache/tauri-smoke-TfD5B6/smoke-result.json`.
   Earlier fODEzZ aggregate process-tree metrics have a PID-reuse sampler bug;
   do not use those totals. The sampler now validates process creation times.
   Default-delay CoFreeUnusedLibraries also did not fix it (EuOKGY: 1 to 10).
   Retained handles refer to exited WebView2 utility processes. Do not manually
   close handles owned by the runtime. Native allocation traces are unavailable
   on this Windows build; an upstream runtime cause is not yet proven.
   Standalone direct WebView2 COM control reproduces 1 to 10 handles without
   Tauri, Wry, Tao, React or app services (raw-webview-sbH9Sd). Balancing COM
   initialization/uninitialization each cycle also fails (raw-webview-sZn14y).
   Guest raw probe initially failed to launch: VCRUNTIME140.dll was missing
   (confirmed via manual inspection). This is NOT a guest lifecycle result. Packaged
   Clipture has no VC runtime DLL imports and launches successfully. build.rs
   now enables Tauri's static VC runtime linkage for direct Cargo builds too;
   its rebuilt probe runs successfully, but confirms the same 1 to 10 handles
   in the clean guest (runtime 152.0.4191.66). All children exit. See current
   guest output/raw-webview-summary.json and raw-webview-details.json.
   Fresh STA thread per cycle also FAILS (raw-webview-t2kTL7: 1 to 10).
   CLIPTURE_RAW_THREAD_PER_CYCLE=1 selects that experiment. Thread teardown,
   COM balancing and removal of all app/framework code do not resolve it.
2. Live signed update/download/install integration still needs verification;
   unit tests and simulated launch-failure recovery are not an installer test.
3. Per-user Electron-to-Tauri crossgrade, custom paths and uninstall now PASS
   in the disposable VM. All-users/force-run also PASS with the fixed engine.
   Rollback and signed packaging
   remain unverified. Never publish an
   update merely because bridge manifest tests pass.
   Sandbox automation now works (network/audio/clipboard off; only isolated
   input/output folders mapped). Prior guest e8409759-330d-420e-8d9c-f110653e6bb9
   no longer exists after interruption; do not reuse its ID.
   `.cache/sandbox-probe-X5ig5V/output` retains passed runtime installation and
   public Electron 1.4.2 custom-path baseline reports. Initial Tauri clean install
   failed: MultiUser initialization overwrote /D. Late GetCommandLine recovery
   also failed. The latest candidate uses a pinned upstream NSIS template with
   two early init hooks to preserve /D; clean install and uninstall now pass.
   Current guest: b7b7026c-6c9c-4106-98f9-02df73dfa94c, profile
   `.cache/sandbox-probe-KYBJPH` (token in input/probe-token.txt).
   Its installer-crossgrade.json passes 15 checks: public Electron 1.4.2 is
   retired from its custom path, one uninstall entry remains, Tauri launches,
   and settings, metadata, custom sound and external clip hashes survive both
   upgrade and uninstall. Installed bytes: 107327584; tray controller private
   bytes: 4542464 in TEST_MODE with no engine (not a recording RAM measurement).
   Hooks inherit legacy scope when updater flags omit it, run a read-only
   data-safety preflight before legacy uninstall, and translate --force-run.
   Machine-wide crossgrade and --force-run launched exactly one Tauri controller,
   but exposed an engine packaging bug: MSVCP140_ATOMIC_WAIT.dll was missing.
   Its failed-test candidate and stuck engine were removed only inside the VM.
   CMake now statically links the VC++ runtime. Native CTest passes. Rebuilt engine
   hash: 102ee5c2f8f5601872183343bd902aa733ec1da6e90c698458d578a2592a199c.
   Direct engine startup/diagnostics/EOF shutdown PASS in the clean guest without
   installing any VC runtime (output/engine-startup.json, exit 0, stderr empty).
   Fixed installer crossgrade/relaunch/uninstall now PASS 17 checks, including
   no remaining direct child processes and all four preserved data hashes.
   output/installer-crossgrade-allusers-force-run.json; installed 107633760 bytes.
   Unsafe crossgrade refusal PASS 9 checks, installer exit 2, Electron and the
   nested clip preserved (output/installer-blocked_crossgrade-allusers.json).
   Installer file 38344130 bytes; this tested candidate predates new audio work.
   Staging now rejects VC runtime DLL imports; PE32/PE32+ normal and delay-import
   regression tests pass. CI also checks the built controller before publishing.
   `node scripts/testing/run-sandbox-probe.cjs --installer-inputs` stages inputs
   for a fresh guest. Installer phases require the fresh probe token and a wsb ID.
   Microsoft offline runtime and public Electron downloads are under `.cache`;
   Microsoft Authenticode and Electron manifest SHA-512 were verified.
4. The release config public key is a LOCAL TEST key, not a production signing
   setup. Do not publish the existing config as a production release.
   Release CI now rejects this key with validate-production-key.cjs. Its focused
   regression tests pass; the production guard currently fails intentionally.
5. Full media segment parity and actual hotkey dispatch need coverage. The
   active Electron save notifications always send an empty thumbnail, so native
   thumbnail feedback is not a missing behavior-parity requirement. Sound formats now pass direct native tests; integrated sound
   playback resource/error/reopen coverage remains.
6. Production defaults/dependency removal and final artifact inventory wait on
   the above gates. Legacy files are deliberately retained.

## Repeatable isolated checks

```powershell
npm.cmd run typecheck
node scripts/migration/test-host-contract.cjs
node scripts/migration/test-renderer-boundaries.cjs
node scripts/migration/test-frame-diagnostics-parity.cjs
node scripts/testing/test-library-snapshot.cjs
node scripts/release/test-electron-bridge.cjs
npm.cmd run build:renderer
$env:TAURI_CONFIG='{"identifier":"app.clipture.smoke","bundle":{"externalBin":[]}}'
cargo test --manifest-path src-tauri/Cargo.toml --lib --offline
cargo build --manifest-path src-tauri/Cargo.toml --features custom-protocol --offline
node scripts/testing/run-tauri-smoke.cjs
node scripts/testing/run-tauri-capture-smoke.cjs
ctest --test-dir build/engine -C Release --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/migration/inventory-artifacts.ps1
```

GUI runners require permission to launch. Capture runner records the display
with audio disabled into a workspace profile, never the daily library. Resource
runs with `CLIPTURE_SMOKE_CYCLES=100` must pass both functional and handle gates.
The executable embeds the renderer: rebuild Rust after changing renderer assets.
Normal packaging requires `node scripts/stage-tauri-sidecars.cjs` first; the
externalBin override above is for isolated development tests only.
