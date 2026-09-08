# Clipture contributor guide

Clipture is migrating from Electron to Tauri 2. The end state is a Windows tray application whose React UI is disposable; recording and saving must continue when no WebView exists.

## Read first

- `docs/architecture.md` describes the process model and source ownership.
- `docs/migration/boundaries.md` contains dependency and file-size guardrails.
- `docs/migration/host-contract.md` defines the compatibility seam.
- `docs/adr/` records decisions that must not be silently reversed.

The Electron implementation remains the behavior reference until a Tauri capability passes its parity test. Do not delete the legacy path merely because a Tauri stub exists.

## Target source map

```text
src/renderer/
  app/                 composition and routing only
  platform/            the only frontend code allowed to call a desktop host
  features/<feature>/  UI, hooks, state, and tests owned by one product feature
  shared/              genuinely reused UI primitives and pure helpers

src-tauri/src/
  app/                  lifecycle, tray, windows, single instance
  engine/               JSON-lines client and sidecar supervision
  settings/             normalization and compatible persistence
  clips/                clip records and library mutations
  library/              discovery/import and library queries
  media/                playback sessions, ranges, thumbnails, FFmpeg
  processes/            process enumeration and icons
  diagnostics/          capture and migration diagnostics
  updates/              signed, capture-aware updates
  notifications/        native overlay
  sounds/               native playback and sound library
  platform/windows/     narrow Windows-specific implementations

engine/                 native C++ capture/encode/mux engine
```

Directories may be introduced incrementally. Preserve these ownership boundaries even while old and new hosts coexist.

## Non-negotiable boundaries

- Renderer features depend on a typed client from `src/renderer/platform`; they never import Electron, Tauri, Node, or Rust details.
- Features expose a small public surface from their root. Do not deep-import another feature's internals.
- `app/` composes features. It does not own media, settings, library, or capture business logic.
- Tauri commands validate/deserialize, call a domain service, and serialize. Business logic does not live in command handlers.
- Rust domains communicate through explicit traits/types, not a global application-state grab bag.
- The C++ engine remains host-neutral. Its stdio protocol is versioned at the boundary; UI concerns do not enter `engine/`.
- File paths from renderer input are untrusted. Media access uses validated paths or opaque session IDs.
- Closing the main window destroys its WebView. Background saves, hotkeys, sounds, updates, and notifications cannot require renderer state.
- Continue using `%APPDATA%\Clipture\data` and the user's existing `saveFolder`; see ADR 0003.

## Context-size rules

Prefer one concept per file. As review triggers, investigate React files above roughly 300 lines and Rust service files above roughly 450 lines. Entrypoints should normally stay below 200 lines. Split by behavior and ownership, not by arbitrary line chunks. Avoid generic `utils`, `helpers`, or catch-all stores.

When changing a cross-process contract, update its type, both adapters, its fixture, and its contract test in the same change. Run:

```powershell
node scripts/migration/test-host-contract.cjs
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/migration/inventory-artifacts.ps1
```

The scripts under `scripts/migration/` are read-only unless their help explicitly says otherwise. They must not start capture, modify user data, or install software.
