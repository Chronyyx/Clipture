# ADR 0009: Signed component runtimes with supervised activation

- Status: Implemented candidate; production rollout gated on isolated signed handoff validation
- Date: 2026-09-19
- Scope: Updater only. No capture, cadence, encoder, replay-buffer or FPS-policy changes.

## Decision

Retain NSIS for initial installation and older Tauri/Electron clients. New signed
builds download immutable runtime directories under
`%LOCALAPPDATA%\Clipture\runtime`. The installed executable remains the entrypoint
and selects a verified newer runtime. ADR 0003 data paths are unchanged.

The existing updater public key authenticates the exact bytes of
`components-v1.json`. Its fixed five-file allowlist, byte counts, SHA-256 hashes
and 1 MiB block hashes authorize the executable, two sidecars and two sounds.
Reject unexpected paths, versions, platforms, URLs, protocol identities,
oversized data and reparse points. Keep verified runtime files read-locked while
in use. Never load an arbitrary executable selected by an unsigned path.

Reuse authenticated whole files and content-matching blocks locally. Request
only missing HTTP ranges, validating Content-Range and every block. A 200
response falls back to a complete bounded download. Reauthenticate the final
file before publishing `pending.json`; partial stages must not alter the active
or pending selection. Transfers obey the existing capture/save gate. This is
not a promise that compressed/relinked executables have small deltas.

The controller performs one delayed startup check, independently of the UI.
Manual checks remain repeatable, including after network failure. A staged
runtime applies on a full launch or an explicit Apply now confirmation; a full
restart clears unsaved replay. A compatible disposable UI worker may use the
staged executable without replacing the engine, but only with an identical
compiled native identity and authenticated matching non-controller files.

## Activation and recovery

1. Reserve the save/update gate. Start an owned supervisor with private pipes.
   It locks the handoff and verifies the pending signed runtime before committing.
2. Exit the old controller normally. The supervisor waits on its exact process
   handle, not a PID polling/reuse heuristic.
3. Launch the candidate, waiting on its private activation pipe before it can
   start its engine/UI. Assign an unnamed Windows job before releasing this gate.
4. Accept health only when both the Tauri event loop and engine configuration /
   hotkey RPC have succeeded, with `engineRunning`. The readiness rendezvous is
   order-independent and emits one acknowledgement. Observe another five seconds
   of controller liveness, then atomically select it and disarm job cleanup.
5. On failure, kill the owned candidate tree, quarantine the exact signed
   runtime and restart the previous executable. A quarantine-write error is
   logged but does not suppress recovery. All candidate entry paths consult
   quarantine, including compatible UI refresh; a different newer release is
   still eligible.

The job extends existing child ownership, rather than enumerating or killing
unrelated processes. Nested jobs preserve WebView2's own job hierarchy, following
[Microsoft's job-object model](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects).
The job is disarmed only on explicit transfer of ownership; failures fail closed.

## Verification and release gates

Tests use temporary directories, local HTTP fixtures, throwaway signing keys and
owned hidden processes. Cover manifest/signature tampering, range fallback and
corruption, shifted-block reuse, partial-stage cleanup, exact-runtime quarantine,
readiness ordering/races, child-tree cleanup and healthy ownership transfer.
Run host contracts, publisher tests, renderer build and Rust host tests. Compile
the release configuration too: debug builds intentionally disable activation.

Before production rollout, use a disposable Windows account/VM for a genuinely
signed old-to-new handoff, compatible UI-only refresh, candidate startup failure,
engine configuration failure, recovery, reboot/shortcut/autostart forwarding and
uninstall behavior. Confirm no duplicate recorder remains and saved clips,
settings and library survive. Do not perform this test on the daily profile.
Do not equate fixture tests or a release compile with this end-to-end gate.

Startup health is not an A/V quality or sustained-capture guarantee. Retention /
garbage collection of old runtime directories and live engine replacement are
not implemented. The legacy installer channel remains available.

### Local validation, 2026-09-19

- Rust host suite: 146 passed, 0 failed, 3 explicitly ignored hardware/audio tests.
- `cargo check --release --locked --offline`: passed (existing warnings remain).
- Renderer typecheck/production build, host contract and renderer/UI-process
  boundaries: passed.
- Runtime-manifest publisher/signature/block tests, Electron bridge, production-key
  guard fixtures and PE runtime-import fixtures: passed.
- Read-only artifact inventory completed without reported errors; diff whitespace
  check passed. No installed runtime was restarted, upgraded or modified.
- No signed application-to-application upgrade or VM rollback claim is made.
