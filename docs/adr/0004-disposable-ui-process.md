# ADR 0004: Isolate the WebView host lifetime from the recorder

- Status: Accepted for the Tauri default; native keyboard/picker UX check outstanding
- Date: 2026-09-07

## Evidence

The ADR 0002 design destroys the WebView and all browser descendants, but the
current WebView2 runtime retains one exited utility-process handle per creation
in the host process. A direct COM example reproduces this without Tauri, Wry,
Tao, React or Clipture services, including in a clean Windows Sandbox. Per-cycle
COM uninitialization, fresh STA threads and an eleven-minute idle period do not
release the handles. The exact upstream allocation cause is not established.

## Decision

Keep the Rust/Tauri tray controller and native engine resident. Create a second
instance of the same executable in a restricted UI-worker role when opening the
interface. The worker owns only its Tauri window, WebView and frontend IPC
adapter. Closing it exits the worker, releasing the complete browser-host
lifetime. It never creates an engine, tray icon, updater scheduler, settings
repository or library writer. This strengthens ADR 0002 without replacing the
Tauri decision or changing the durable-data owner.

Use private inherited stdin/stdout pipes, not a listening network endpoint.
Version and bound every frame. The resident controller dispatches only an
explicit allowlist of existing host operations; arbitrary shell/filesystem
commands are not introduced. Binary media replies remain bounded, and media
session ownership is tied to the worker generation rather than renderer input.
The frontend continues to depend on the existing typed platform client.

On disconnect, release worker-owned playback sessions and event subscriptions.
An accepted save belongs to the controller and must finish even if the worker
exits. A new worker obtains snapshots rather than replaying an unbounded event
history. Duplicate Open requests focus the current worker. Controller exit
reaps only its owned worker and engine; no process-name-based cleanup.

## Acceptance gates

- First prove 100 disposable native WebView processes leave stable resident
  handles and memory, with zero WebView descendants or worker orphans at close.
- Preserve all existing command, media, event, picker and settings tests.
- Save during close, kill/reopen the worker during capture, and verify recording
  and metadata ownership remain exclusively in the resident controller.
- Exercise malformed frames, oversized payloads, queue saturation, EOF,
  bootstrap failure, stale replies, and shutdown races.
- Repeat actual application 100-cycle resource and installer/relaunch tests.

Acceptance is not based solely on the raw-process experiment. Actual application
run `tauri-smoke-xGCCn0` passed 100 cycles and all 15 functional checks per cycle,
with zero retained Process-handle growth and no worker/browser orphans. Capture
run `tauri-capture-FBnv3A` passed 29 checks, including save during close, worker
crash/reopen, duplicate Open races and unchanged recording-engine generation.
The clean Sandbox candidate also passed clean install, both cross-grade scopes,
force-run relaunch, real HTTPS signed updater install and rollback (see checkpoint).

Each worker is assigned to an owned, non-inheritable kill-on-close Windows job
before bootstrap, so delayed browser descendants cannot survive worker teardown.
Backpressure coalesces bounded state hints while command replies remain reliable.

Native picker callback semantics, physical hotkeys, and picker focus/modality
have been verified directly on the host system. The architectural and
repository-default decision is accepted based on the verified evidence above.
Never manually close runtime-owned handles, disable browser security, forcibly
unload WebView DLLs, or pin an obsolete runtime. `CLIPTURE_UI_PROCESS=0` retains
the old in-process path solely as a migration diagnostic oracle.
