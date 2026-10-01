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

The disposable UI host retains a six-request concurrency ceiling, reserving two
slots from background work for playback setup/release. Explicit pre-dispatch
`UI host is busy; retry shortly` failures are classified by the platform adapter.
The player retries those failures at most three times (100/250/500 ms), cancelling
pending retries when the clip changes or the player unmounts. Timeouts and other
errors are not retried. Busy errors do not suggest missing command registration.
Run `node scripts/migration/test-playback-busy.cjs` alongside the host contract.

- `invoke` operations return a promise and exactly one result or rejection.
- `send` operations are deliberately fire-and-forget.
- `event` registrations synchronously return an idempotent unsubscribe function.
- Host adapters normalize transport errors into useful `Error` messages; feature code must not branch on Electron/Tauri error shapes.
- Events are hints that state changed. On reconnect or suspected loss, query a fresh snapshot rather than relying on replay of every event.
- Closing a renderer releases all subscriptions and playback sessions owned by it.

## Evolution

During migration, keep the v1 facade stable. New optional functionality may be additive. A breaking rename/removal requires a new fixture version and coordinated adapters. Once feature clients replace the large facade, retain a compatibility adapter until Electron is removed.

The engine protocol fixture in `engine-protocol.v1.json` is separate because it is a private host-to-sidecar boundary. Its request IDs are controller-generated positive integers; stdout replies are either `{id,payload}` or `{id,error}`, and the native hotkey is an unsolicited event.

## Friend sharing client

Friend sharing is a separate, additive `SharingApi` (`src/shared/sharing.ts`),
not part of the v1 `CliptureApi` facade, because only the Tauri host implements
it (ADR 0011). `src/renderer/platform/sharing-client.ts` selects the Tauri
adapter, the browser mock (`?preview`, `&sharing=off`), or an unsupported
adapter that reports `supported: false`. `sharing://changed` is a payload-free
hint relayed to the UI worker. `sharing_stream_url` takes its owner from the
connection, like playback; the Tauri adapter returns `StreamUrls`, the video
route and the mixed-audio route beside it (`/v1/remote/<id>/audio`, valid once
`SharedClip.allAudioReady`). `SharedClip.streamed` carries the host's own record
of streamed byte ranges, and `cancelDownload` stops an "Add to library" copy.
Media replies relayed to the UI worker never exceed the pipe's 4 MiB body
limit; an oversized reply fails that request instead of closing the window.
`scripts/migration/fixtures/sharing-contract.v1.json`
pins the method-to-command map, and `test-sharing-contract.cjs` (run by the host
contract) checks the type, adapters, `lib.rs` registry, dispatcher arguments and
event relay together.

## Contract-test expansion points

`ClipSettings.fps` accepts 24, 30, 60 and 120. Values above 60
are exposed as experimental recording targets, not guaranteed unique-frame
throughput. Both frontend adapters share the FPS normalizer; the Electron host
and Rust settings normalization preserve the same choices. Unsupported values
fall back to a conservative 30 FPS. Fresh installs default to 60 FPS, a
60-second clip length and automatic bitrate; stored profiles keep every value.
`ClipSettings.defaultAppSourcesVersion` is host-managed: the Tauri host adds the
default browser and Discord as separate app tracks once (never re-adding one
the user removed) and raises the marker; adapters round-trip it untouched. `captureFpsOptions` in the host fixture
pins the dropdown, normalization and Rust engine-config serialization; isolated
settings-store tests verify higher values survive disk save/reload.

`ClipSettings.saveInPlace` is additive and defaults to true when absent; explicit
false restores the legacy RAM/spill replay path. `ClipSettings.saveInPlaceOverlap`
is additive, defaults to true and only matters while `saveInPlace` is on: true
keeps overlapping windows (retained footage is backfilled from the saved clip
into the live arena in the background); explicit false consumes the window, so
each save starts where the last successful engine save ended. Both renderer
adapters normalize both fields. Engine `configure` carries `saveInPlace`,
`saveInPlaceOverlap` and `saveFolder`, so native background recording chooses
storage without depending on a WebView. With overlap off, the engine consumes
the visible window only after engine save success; subsequent host processing
failure preserves its source MP4 but does not roll that boundary back. Settings
defaults and opt-outs are pinned in the host fixture, adapter tests and Rust DTO
tests; the configure fields are pinned in the separate engine fixture.

Folder settings are host-owned in the Tauri host. `saveSettings` ignores
`importedVideoDirectories` and `importedVideoTitles` from the renderer (they
change only through import, rename and delete), and accepts a new `saveFolder`
only when it equals the folder the native picker (`selectFolder`) just
returned; that grant is single-use. A compromised page therefore cannot widen
which files the library may list, rename or delete. The renderer already only
changes these through those commands, so no adapter change was needed.

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
