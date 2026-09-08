# ADR 0001: Use Tauri 2 as the desktop host

- Status: Accepted
- Date: 2026-09-03

## Context

Electron bundles a Chromium/Node process tree even though Clipture's capture and encoding already live in a native C++ process. The existing React/Vite renderer is valuable and should not be rewritten merely to change the host. Clipture is Windows-first and needs tray lifecycle, native sidecars, an updater, dialogs, startup registration, notifications, and controlled media delivery.

## Decision

Use Tauri 2 with a Rust controller as the production desktop host. Keep the React/Vite UI and run the existing C++ engine as a private sidecar over JSON-lines stdio. Port behavior by domain behind the typed frontend host contract while Electron remains a temporary reference implementation.

Rust owns lifecycle and privileged work. Tauri commands remain thin transport adapters. Use a minimal capability allowlist and do not expose generic shell, filesystem, or process execution to the renderer.

## Consequences

- Installed size and tray-idle memory no longer include bundled Chromium and Node.
- The team maintains Rust controller code and Windows integration in addition to C++ and TypeScript.
- An open UI still incurs WebView2 processes; ADR 0002 limits that cost to the time the UI is visible.
- Some Electron services, especially updater and media streaming, require explicit replacements rather than mechanical IPC renames.
- Electron-to-Tauri upgrade and rollback need dedicated installer tests.

## Rejected alternatives

- Raw Win32 plus WebView2 offers marginally more control but substantially more host plumbing.
- A fully native UI gives the smallest visible-UI footprint but discards the existing frontend and greatly expands migration risk.
- Wails, Neutralino, Qt, Flutter, and Avalonia do not offer a better combination of current UI reuse, mature native hosting, and footprint for this codebase.

