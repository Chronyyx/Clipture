# Clipture architecture

## Goal

Clipture is a Windows-first, NVENC-first clipping application. Its steady-state footprint should contain only a small native controller and the native capture engine. The React interface is created on demand and destroyed when closed.

## Target process model

```text
Clipture.exe (Rust/Tauri controller, resident)
  |-- tray, lifecycle, settings, hotkeys, library, updater
  |-- JSON-lines over private stdin/stdout
  +-- clipture_engine.exe (resident capture/encode/mux sidecar)

On demand only:
  clipture.exe --ui-worker (private pipes; no durable services)
    +-- WebView2 tree (React UI; entire worker job ends on close)
  ffmpeg.exe workers (bounded media/transcode jobs)
  native notification overlay (short lived)
```

The controller, not the renderer, owns durable application state. Opening a UI window requests a fresh snapshot and subscribes to events. Destroying that window cannot cancel a capture or save.

## Migration state

Tauri is the default development, launch and package path. Both hosts implement
the typed browser-facing contract; Electron remains an explicitly named behavior
reference, not an installed runtime dependency. Real isolated tests verify
capture without a WebView, 100 disposable UI lifetimes, installer cross-grade,
signed updater handoff and rollback. Physical keyboard delivery and native picker
focus/modality have been verified directly on the host system. See [the checkpoint](migration/checkpoint.md)
for exact evidence.

The native engine boundary is already host-neutral: one JSON object per line on stdin, correlated replies on stdout, and diagnostic logs on stderr. See `scripts/migration/fixtures/engine-protocol.v1.json`.

## Ownership by domain

| Domain | Resident owner | Renderer responsibility |
| --- | --- | --- |
| Capture, encode, replay buffer, mux | C++ engine | Display state and initiate a save |
| Engine supervision and protocol | Rust `engine` | None |
| Settings and compatibility | Rust `settings` | Edit a draft and submit it |
| Clip records and filesystem mutations | Rust `clips` | Query and request explicit actions |
| Library discovery/import | Rust `library` | Filters, selection, and presentation |
| Playback, ranges, thumbnails, FFmpeg | Rust `media` | Playback controls and session lifecycle |
| Tray, window lifecycle, startup | Rust `app` | None |
| Updates | Rust `updates` | Present state and ask for actions |
| Save feedback | Rust `sounds` and `notifications` | Optional preview while UI is open |
| Diagnostics | Rust `diagnostics` plus engine | Presentation/export request |

## Data flow

1. The Rust controller becomes the single application instance and resolves the legacy data directory.
2. It starts the engine as a private child and sends configuration over JSON-lines.
3. The engine maintains the replay buffer independently of any UI.
4. A native hotkey or tray command asks the controller to save. The controller coordinates the engine, clip metadata, sound, and notification.
5. When the user opens Clipture, the controller starts one disposable UI worker
   and supplies snapshots through private, versioned, bounded pipes. The worker
   creates the WebView but never acquires durable service ownership.
6. Playback uses an opaque, short-lived media session. Renderer-provided arbitrary paths never become open loopback URLs.

## Source boundaries

The renderer is feature-oriented and the Rust host is domain-oriented. Detailed import rules and intended directory layouts are in `migration/boundaries.md`. The stable migration seam is the `CliptureApi` interface in `src/shared/types.ts`, recorded in `migration/host-contract.md`.

## Invariants

- No Electron, Node, WebView2, or FFmpeg process remains at tray idle.
- One controller owns one engine child; shutdown and unexpected-exit behavior are explicit.
- Closing/reopening the UI does not change capture state or leak event subscriptions.
- User settings, clip metadata, custom sounds, and save locations survive Electron-to-Tauri upgrades and rollback.
- The package contains one engine and one FFmpeg binary.
- Cross-process inputs are bounded and validated. External file operations require a path authorized by application state or a native picker.

## Decisions

- [ADR 0001: Tauri 2 desktop host](adr/0001-tauri-2-desktop-host.md)
- [ADR 0002: Lazy, disposable WebView](adr/0002-lazy-disposable-webview.md)
- [ADR 0003: Preserve the legacy data path](adr/0003-preserve-legacy-data-path.md)
- [ADR 0004: Isolate the WebView host lifetime](adr/0004-disposable-ui-process.md)
