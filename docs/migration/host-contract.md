# Host contract

`CliptureApi` in `src/shared/types.ts` is the temporary compatibility seam between React and either desktop host. The checked-in fixture at `scripts/migration/fixtures/host-contract.v1.json` pins its method names, transport kinds, and Electron channel mapping.

Run the non-mutating static check with:

```powershell
node scripts/migration/test-host-contract.cjs
```

The test reads source and fixtures only. It does not import Electron, start the engine, open a window, inspect `%APPDATA%`, or invoke any host method.

## Semantics

Diagnostics adapter refresh failures reject instead of synthesizing an offline
engine. The UI retains the last received snapshot with an explicit delayed/stale
label until polling recovers. Actual degraded/offline snapshots still replace it.

- `invoke` operations return a promise and exactly one result or rejection.
- `send` operations are deliberately fire-and-forget.
- `event` registrations synchronously return an idempotent unsubscribe function.
- Host adapters normalize transport errors into useful `Error` messages; feature code must not branch on Electron/Tauri error shapes.
- Events are hints that state changed. On reconnect or suspected loss, query a fresh snapshot rather than relying on replay of every event.
- Closing a renderer releases all subscriptions and playback sessions owned by it.

## Evolution

During migration, keep the v1 facade stable. New optional functionality may be additive. A breaking rename/removal requires a new fixture version and coordinated adapters. Once feature clients replace the large facade, retain a compatibility adapter until Electron is removed.

The engine protocol fixture in `engine-protocol.v1.json` is separate because it is a private host-to-sidecar boundary. Its request IDs are controller-generated positive integers; stdout replies are either `{id,payload}` or `{id,error}`, and the native hotkey is an unsolicited event.

## Contract-test expansion points

`ClipSettings.saveInPlace` is additive and defaults to true when absent; explicit
false restores overlapping replay saves. Both renderer adapters normalize it.
Engine `configure` carries `saveInPlace` and `saveFolder`, so native background
recording chooses storage without depending on a WebView. The engine consumes
the visible window only after engine save success; subsequent host processing
failure preserves its source MP4 but does not roll that boundary back. Settings
defaults and opt-out are pinned in the host fixture, adapter tests and Rust DTO
tests; the configure fields are pinned in the separate engine fixture.

The additive `ClipRecord.segmentAudioTracks` field describes each segment's
actual audio stream order; `audioTracks` is their ordered union. Empty entries
mean no audio exists in that segment. The host pads absent spans and remaps by
identity before concatenation, then clears all segment metadata after publishing
the final clip. Older records without the field retain the legacy same-layout
assumption. The engine now reports muxed logical stream identities rather than
guessing them from input packets that may have been dropped. Both renderer
adapters preserve this recovery metadata; the shared fixture and Rust round-trip
test pin it. The temporary Electron reference refuses changing audio layouts
instead of silently concatenating mismatched streams.

The current test is intentionally static and hardware-free. Add these suites beside each implementation as it becomes available:

- Adapter conformance using one shared fake transport.
- Serialization round trips for every command DTO.
- Subscription/unsubscription and destroyed-window cleanup.
- Engine chunking, malformed-line, timeout, and child-exit behavior.
- Path authorization for media sessions and file mutations.
- Golden normalization fixtures for legacy settings and clip records.

Keep all fixture paths synthetic (for example `C:\Fixture\...`). Tests must direct any necessary writes to an isolated temporary directory, never the real Clipture data directory.
