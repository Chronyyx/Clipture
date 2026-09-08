# Migration boundaries

This document is a routing map for humans and coding agents. Put new behavior with the domain that owns its invariant; do not grow another frontend or host monolith.

## Frontend layout

```text
src/renderer/
  app/
    App.tsx                 composition only
    bootstrap.tsx
  platform/
    client.ts               host-neutral interface/re-export
    electron-adapter.ts     temporary compatibility adapter
    tauri-adapter.ts
    mock-adapter.ts         deterministic browser/test adapter
  features/
    library/
    player/
    settings/
      capture/
      audio/
      appearance/
    diagnostics/
    updates/
  shared/
    components/
    hooks/
    styles/
```

Each feature owns its components, hooks, state transitions, styles, and focused tests. A feature root may expose `index.ts`; callers use that surface. Cross-feature coordination belongs in `app/` or a small explicit workflow, not in either feature's internals.

Only `platform/` may touch `window.clipture`, `@tauri-apps/*`, Electron compatibility objects, command names, or desktop event names. Components consume typed domain methods and unsubscribe functions. `mock-adapter.ts` must allow UI work without Rust, Electron, capture hardware, or real user files.

Global CSS is limited to reset, tokens, typography, and themes. Feature layout and states remain with the feature. `shared/` is promoted code used by multiple features, not a staging area.

## Rust layout

```text
src-tauri/src/
  main.rs / lib.rs          bootstrap and dependency composition only
  app/                      lifecycle, tray, windows, single instance
  engine/                   protocol codec, client, supervision
  settings/                 defaults, normalize, repository
  clips/                    records, rename/delete/reveal
  library/                  list, discover, import, change events
  media/                    sessions, byte ranges, mix, thumbnails
  processes/                enumeration and executable icons
  diagnostics/              snapshots and export
  updates/                  state machine, transfer policy, install
  notifications/            native overlay
  sounds/                   playback, import, enumeration
  platform/windows/         OS-specific primitives
```

Tauri command functions are transport adapters: deserialize, validate, authorize, call one service, map the error. They do not read files, spawn FFmpeg, or encode business rules directly. Services receive narrow dependencies through constructors/traits. Avoid a single mutex-protected mega-state; lock ownership must be domain-local and no lock may be held across an `await` unless documented.

Windows APIs belong in `platform/windows/` when they are reusable primitives. A domain-specific Windows implementation may remain beside its trait when moving it would obscure ownership.

## Native engine boundary

The engine accepts newline-delimited JSON requests and emits correlated replies:

```json
{id: 7, type: getDiagnostics}
{id: 7, payload: {engineRunning: true}}
```

Unsolicited events have an `event` field and no correlation requirement. Stderr is logs only; stdout is protocol only. The client must tolerate chunked lines, reject malformed/oversized messages, time out requests, and fail all pending requests when the child exits.

Protocol changes are additive within a version. Update `engine-protocol.v1.json` and a codec test before changing both sides. Do not infer message types with substring matching in new code.

## File and API sizing

These are review signals, not reasons to split cohesive logic badly:

- App/entrypoint: aim below 200 lines.
- React component or hook: investigate above 300 lines.
- Rust service/module: investigate above 450 lines.
- Contract DTOs may be longer when a single schema is the useful unit.

Prefer names such as `playback_session.rs` or `useClipSelection.ts` over `utils`, `helpers`, `manager`, and `misc`. Split along state ownership, side effects, or independently testable policy.

## Allowed dependency direction

```text
renderer app -> renderer features -> renderer shared
             -> renderer platform interface
renderer platform adapters -> Electron or Tauri transport

Tauri commands/app -> domain services -> domain types/policies
domain services -> narrow platform traits
platform/windows -> Windows APIs
Rust engine client -> C++ engine protocol
```

Domain services never depend on Tauri window types. The engine never depends on Rust/Tauri or renderer types. Shared DTOs cannot acquire filesystem or process behavior.

## Change checklist

For a behavior being ported:

1. Identify its Electron behavior and persistent-data effects.
2. Add or update a deterministic fixture/test at the boundary.
3. Implement the Rust domain service and thin command.
4. Implement the Tauri adapter without changing feature code.
5. Test with the WebView absent, then open, close, and reopen it.
6. Compare artifacts and process footprint.
7. Remove the Electron implementation only after parity and upgrade coverage exist.

