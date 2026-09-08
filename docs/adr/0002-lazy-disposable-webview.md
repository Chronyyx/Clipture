# ADR 0002: Make the WebView lazy and disposable

- Status: Accepted
- Date: 2026-09-03

## Context

Hiding a browser window retains its renderer and GPU costs. Clipture spends most of its life recording in the tray, where the library UI is unnecessary. Saves and their feedback must also work when no renderer has ever opened or after it has closed.

## Decision

Start Tauri without a main WebView. Create one main window only when the user selects Open from the tray, activates the single-instance application, or takes another explicit UI-opening action. A user close destroys the window and WebView instead of hiding it.

The Rust controller owns all durable state and long-running operations. A newly created renderer requests snapshots, then subscribes to change events. Native code owns tray actions, hotkey saves, sound playback, and the capture-excluded notification overlay. Renderer-owned playback sessions and subscriptions are released when its window is destroyed.

## Consequences

- Tray idle has no WebView2 descendant process.
- UI startup has a small, accepted cold-start cost.
- Renderer-local state is ephemeral. Unsaved form drafts may require an explicit close warning, save, or discard policy.
- Events cannot be the sole state store; reconnect always begins with snapshots.
- Window open/close races and repeated creation require lifecycle tests, including 100-cycle leak checks.

## Verification

Measure the controller process tree after startup, with UI open, and after close. WebView2 descendants must disappear shortly after close while hotkey capture and save remain functional.

