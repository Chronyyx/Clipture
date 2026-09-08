# ADR 0003: Preserve the legacy Clipture data path

- Status: Accepted
- Date: 2026-09-03

## Context

Electron currently stores application data beneath `app.getPath(userData)/data`, which for the shipped Windows identity is `%APPDATA%\Clipture\data`. It contains `settings.json`, `clips.json`, imported sounds, caches, and diagnostics. Settings also point to the user's clip save folder and imported video roots. Tauri's identifier-derived default directory may differ, which could make an upgrade appear to lose the library and settings.

## Decision

The Tauri host explicitly resolves and continues using `%APPDATA%\Clipture\data`. It does not silently move or copy the directory to an identifier-derived Tauri path. Existing `settings.json`, `clips.json`, `sounds\`, and user-selected external paths retain their meaning.

Readers tolerate missing optional/new fields and normalize in memory. Writers preserve compatible fields, use atomic replacement, and avoid rewriting data during read-only startup. Schema changes that cannot round-trip through the rollback version require an explicit versioned migration, backup, rollback test, and ADR update.

Tests use fixture or temporary paths. They must never discover or open the real `%APPDATA%\Clipture\data` unless an explicit manual migration test requests it.

## Consequences

- Electron-to-Tauri upgrade can retain settings and the library without a one-time destructive migration.
- The path is a compatibility promise even if the product identifier changes.
- Tauri path APIs cannot be used blindly for application data.
- Clean install, upgrade, malformed-data, interrupted-write, and rollback cases need tests.
- Caches may be rebuilt, but only after distinguishing them from user-owned sounds and metadata.
