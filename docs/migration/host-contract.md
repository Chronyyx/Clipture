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
`shareClip` returns `SharedClipResult` (`shareId` plus the snapshot) so the UI
can follow that share. `answerSharedClip` accepts or declines an incoming clip.
`SharedClip.answer` is `pending` for clips waiting on a decision (inbox) or a
friend's answer (outbox). Outbox clips carry `transfer` (bytes sent, purpose,
state, rate) and `saved` once the friend confirmed a verified copy. Both sides
carry `availableUntilMs`, the end of the 15-minute window.
`setFriendNickname` stores a local-only name for a friend; the host then
returns it as that friend's `name` (and in every clip's `friendName`), with
`Friend.nickname` set, so the renderer never has to choose between the two.
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
false restores the legacy RAM/spill replay path. While `saveInPlace` is on, each
save consumes the window, so it starts where the last successful engine save
ended (ADR 0026 removed the former `saveInPlaceOverlap` field; old profiles that
still carry it are normalized without it). Both renderer adapters normalize
`saveInPlace`. Engine `configure` carries `saveInPlace` and `saveFolder`, so
native background recording chooses storage without depending on a WebView. The engine consumes
the visible window only after engine save success; subsequent host processing
failure preserves its source MP4 but does not roll that boundary back. Settings
defaults and opt-outs are pinned in the host fixture, adapter tests and Rust DTO
tests; the configure fields are pinned in the separate engine fixture.

`UpdateState.disabled` is additive and optional: the Tauri host sets it (with
`message` explaining why) for builds that cannot update themselves, from the
first `getUpdateState` on, and omits it otherwise. The title bar then shows
"Updates off" instead of a check button. Adapters pass the state through
unchanged; the Electron reference never sets it.

Folder settings are host-owned in the Tauri host. `saveSettings` ignores
`importedVideoDirectories` and `importedVideoTitles` from the renderer (they
change only through import, rename and delete), and accepts a new `saveFolder`
only when it equals the folder the native picker (`selectFolder`) just
returned; that grant is single-use. A compromised page therefore cannot widen
which files the library may list, rename or delete. The renderer already only
changes these through those commands, so no adapter change was needed.

`trimClip(id, startSeconds, endSeconds)` keeps that part of one of Clipture's
own single-file clips and permanently deletes the rest from its file
(`trim_clip`, ADR 0013). The id is resolved through the authorized library
snapshot; imported videos and multi-part clips are refused. It resolves with
the kept length and the file size before and after, and emits
`library:changed`. The Electron reference rejects it.

`mixClipAudio(id, gains)` re-encodes each audio track of the same kind of clip
at `gains[k]` (by track position: 0 is silence, 1 leaves the track as it is,
at most 10, +20 dB) in its file (`mix_clip_audio`, `media::mix`). Video and unchanged
tracks are not touched. It resolves with the number of tracks re-encoded and
the file size before and after, and emits `library:changed` when anything
changed. The Electron reference rejects it. `clipPlaybackUrl` results gain the
optional `audioTrackIndexes` and `trackChunkSeconds`; the Tauri host's audio
chunks accept `tracks=1` for one stereo pair per listed track, which the
player's mixer balances live. Such a chunk may be at most `trackChunkSeconds`
long, so it stays under the UI process transport's 4 MiB frames. The optional
`waveformUrl` serves the clip's audio in audiowaveform's JSON format (version
2, one channel, 8-bit min/max pairs per 10 ms) for the trim editor's waveform
views; the host builds it on request from the clip's audio packets alone; the Electron reference omits it.

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
