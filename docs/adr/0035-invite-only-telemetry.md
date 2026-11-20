# ADR 0035: Invite-only telemetry to a self-hosted HTTPS endpoint

Date: 2026-11-14

Status: experimental.

## Context

Diagnosing capture problems today needs a user to notice a bad clip, export
diagnostics and send the file. A few friends offered to send recording health
continuously, so problems show up across days of real games on hardware other
than the 3080 PC. Telemetry must not reach anyone who did not ask for it, must
not need a separate build, and must survive normal updates.

The diagnostics export (`DiagnosticsReport`, report version 6) is not suitable
as-is. Its engine section names the display (`status`, `display`,
`captureTargetName`) and the microphone (`microphoneDevice`), and the save
timing section carries a log path. Telemetry has its own shape, built from an
allowlist.

## Decision

### One build, off by default

Every build contains the telemetry client, disabled, and no server address.
It starts only after the user pastes an invite code in the Telemetry panel at
the bottom of the Diagnostics tab and confirms, or runs
`clipture.exe --telemetry-enroll <code>`.
Stop deletes the key and the unsent journal; the server keeps what it already
received until the admin deletes the device.

### Enrollment and authentication

1. The admin tool issues an invite for a named friend:
   `ctel1.<base64url(server URL)>.<token>`, where the token is 16 random bytes.
   The server stores `sha256(token)`, single use, expiring after 24 hours.
2. Clipture generates an Ed25519 key pair and sends
   `POST /v1/enroll { invite token, public key, install summary, signature }`;
   the signature over `clipture-telemetry-enroll-v1\n<token>\n<public key>`
   proves possession. The server marks the invite used and returns a device id.
3. The private key is stored with DPAPI (`CryptProtectData`, current user),
   so a copied data folder is useless on another account or PC.
4. Each upload carries `X-Clipture-Device`, `X-Clipture-Timestamp` (Unix ms)
   and `X-Clipture-Signature`: Ed25519 over
   `clipture-telemetry-batch-v1\n<device>\n<timestamp>\n<sha256 hex of body>`.
   The server rejects timestamps more than 10 minutes off and unknown or
   revoked devices. A replayed batch is harmless: its sequence numbers are
   already stored (below).
5. 401/403 marks the enrollment revoked on the client; it stops sending and
   Settings says so. Codes are never reusable, so a leaked one is worthless
   once redeemed or expired.

URLs must be `https://`, except loopback addresses (development and the
sandbox test).

### What is recorded

Nothing is serialized unless this section lists it.

Every record is one JSON line with an envelope:

```json
{ "seq": 1842, "t": "2026-11-14T21:04:00.000Z", "run": "r-…", "session": "s-…", "type": "minute", … }
```

`seq` increases by one per record for the life of the enrollment. `run` is one
start of Clipture; `session` is one stretch of the replay buffer running
(engine running), or null outside one.

| type | when | contents |
| --- | --- | --- |
| `run_start` | app start | version, previous version (an update), whether the previous run ended cleanly, Windows build, CPU model and threads, memory, GPU and driver, hardware GPU scheduling, displays (size, refresh, HDR, primary), recording settings (below), whether Xbox Game Bar records games in the background (`AppCaptureEnabled` and `HistoricalCaptureEnabled` under `HKCU\Software\Microsoft\Windows\CurrentVersion\GameDVR`, since 1.9.3) |
| `run_end` | quit, update, restart | reason |
| `session_start` / `session_end` | engine starts/stops recording | capture backend, encoder, codec, source/output resolution, target fps; at the end: minutes, frames dropped, saves, warnings, errors |
| `minute` | each clock minute of a session, from the 2-second engine samples | samples; output and capture fps (mean and minimum) and the game's present rate (mean of per-sample rates from the engine's present counter since 1.9.3; before, the average since capture started; capped at twice the recording rate, ADR 0031); dropped frames and each non-zero counted reason (`frame_definitions.json`); dominant reason; worst capture pressure; seconds degraded; p95 maxima of NVENC call, encoder queue residence, capture processing and input mapping; process and adapter VRAM maxima (GPU busy is not sampled: reading it costs the engine a counter collection); replay archive queued and RAM-fallback bytes maxima; GPU scheduling class; seconds at realtime GPU priority, with a known game in front, and with a wanted raise held by a gate; dropped frames at realtime and at normal; the known game in front for most of the minute (its exe name, since 1.9.3) |
| `event` | immediately when it happens | `level` (info, warn, error), `code`, optional scrubbed `message`, small `data` map |

An `incident` carries the 2-second samples around a detected problem: the
up to 30 samples (60 s) the recorder already holds in memory, the trigger,
and 15 samples (30 s) after it, during which the host asks the engine for the
costlier sample that includes GPU load. Each sample has recorded, captured
and game (desktop present) fps, dropped frames by reason, capture pressure,
NVENC, encoder-queue, capture and input-mapping latencies, the longest gaps
between source frames, recorded frames and published frames, scheduler
lateness, queued frames, NVENC frames and GPU packets in flight, process and
adapter VRAM and the VRAM budget, replay bytes queued, the GPU scheduling
class, whether a known game was in front, whether the GPU policy wanted a
raise, and the gate holding it. Triggers, 10 seconds into a session:

| trigger | condition |
| --- | --- |
| `fps_drop` | the game presents at 90% or more of the target, the recording at under 80% |
| `output_stall` | over 200 ms between recorded frames while the game presents at 90%+ of the target |
| `drop_burst` | 10 or more frames dropped in one sample |
| `capture_slots` | 3 or more capture-slot drops in one sample (ADR 0022's starvation) |
| `encoder_backpressure` | 3 or more encoder-backpressure drops in one sample |
| `gpu_starvation` | the engine's lossy-starvation window count rose |
| `vram_pressure` | adapter VRAM use at 95% or more of the budget |
| `replay_ram_fallback` | the replay archive started spilling to RAM |
| `pressure_critical`, `encoder_hang`, `recording_halted`, `replay_write_failure`, `capture_access_lost`, `degraded` | as the events of the same names |

A game that runs below the target (a menu, a heavy scene) is not lag: the
recording follows its source. Triggers during an open incident join it; after
it closes the next waits 2 minutes, and there are at most 10 an hour.

Recording settings sent: fps, bitrate and auto bitrate, NVENC preset, clip
length, resolution preset, monitor mode, save-in-place, GPU priority boost,
admin-game hotkey, the number of audio source rules, which audio sources are
on (booleans), microphone isolation and noise gate on/off. Not sent: folders, device names or ids, process lists,
hotkeys.

Event codes:

| code | level | source |
| --- | --- | --- |
| `save` | info | every hotkey/UI save: duration ms, clip seconds, fps, resolution, encoder; the game name only when the clip was matched as a game |
| `save_failed` | error | save result with its scrubbed message |
| `drop_burst` | warn | 10 or more frames dropped in one 2-second sample, with the reasons |
| `capture_backend_changed` | info | active backend differs from the previous sample |
| `capture_fallback` | warn | `captureFallbacks` increased; `captureFallbackReason` |
| `capture_access_lost` | warn | `captureAccessLosses` increased |
| `capture_recreated` | info | `captureRecreationSuccesses` increased |
| `replay_write_failure` | error | `replayArchiveWriteFailures` increased |
| `pressure_changed` | info/warn | capture pressure changed (warn when not healthy) |
| `degraded` | warn | engine reports degraded (start and end) |
| `gpu_users` | info | after a minute's first drop burst, what else used the GPU in a 0.5 s sample of Windows' per-process GPU engine counters: the app kinds that were encoding, decoding and rendering (a fixed list such as `Discord`, `browser`, `NVIDIA overlay/ShadowPlay`, `Clipture window`; a Windows service that encodes is `Xbox Game Bar background recording`, the Game DVR service in svchost; any other process is `other app`, its name never sent; each kind's share is its busiest engine of that type, not a sum over engines), and the recorder's own encode and 3D shares. Measured on an RTX 3080: the first probe of a run costs about 0.5 s of CPU on its own thread while Windows loads the counter providers, later ones about 16 ms. Since 1.9.3 |
| `gpu_priority_changed` | info | scheduling class changed: from, to, the reason (a policy decision such as `gpu quiet` or `raise ineffective`, or a gate such as `encoder setup`) and the known game's exe name the engine saw in front |
| `encoder_hang` | warn | encoder watchdog recovered the encoder |
| `recording_halted` | error | encoder watchdog gave up |
| `engine_lost` | error | engine stopped running inside a run without a requested stop, or missed three samples in a row |
| `engine_unavailable` | error | the engine has not answered 30 seconds after start (the run summary is then sent without GPU details) |
| `crash` | error | a new `crash.log` entry, read on the next start |
| `unclean_exit` | error | the previous run has neither `run_end` nor a crash entry |
| `host_warning` / `host_error` | warn/error | the host's own `tracing` warnings and errors: target and scrubbed message; identical ones within a minute are merged with a count, at most 30 per minute |

Scrubbing (messages only, every field above is structured): Windows and UNC
paths become `<path>`, the user and computer names `<user>`/`<host>`, e-mail and
IP addresses `<email>`/`<ip>`; messages are cut at 300 characters.

The Telemetry panel shows the last uploaded batch, decoded ("Show what's
sent").

### Client storage

`%APPDATA%\Clipture\data\telemetry\`, outside `settings.json` (ADR 0003), so
updates keep it and settings normalization never touches it:

- `enrollment.json`: server URL, device id, friend name, public key, the
  DPAPI-protected private key, state (`active`, `revoked`).
- `state.json`: next `seq`, last run id and whether it ended, last version,
  read offset into `crash.log`, last uploaded batch summary.
- `journal\<first seq>.jsonl`: append-only segments. Each record is written
  and flushed when it is produced, so a crash or abort (release builds abort
  on panic) loses nothing already recorded. The open segment is sealed at
  256 KB or after 60 seconds, and within 10 seconds of an error event.

The uploader sends sealed segments oldest first, one per request, gzip, and
deletes a segment only after a 2xx; a 400 or 413 (the server will never take
it) deletes it and records a `gap`. Failures back off up to 30 minutes. The journal is
capped at 8 MB: past that the oldest segment is deleted and a `gap` record
names the lost sequence range. Recording runs in the Rust host on its own
low-priority work, never in the engine or the frame path, and needs no
WebView.

### Server

A separate program in the private repository `Chronyyx/clipture-telemetry-server`,
not part of the app or its installer: Rust (axum, rusqlite with bundled
SQLite, ed25519-dalek). The wire format, signing strings and invite parsing
live here in `tools/telemetry-protocol/`: the app uses it as a path crate and
the server as a git dependency on this repository, so they cannot drift.

- The ingest listener (`/v1/enroll`, `/v1/batches`) binds 127.0.0.1; IIS
  terminates TLS and reverse-proxies to it (URL Rewrite + ARR). Bodies are
  limited to 1 MB compressed and 8 MB decompressed; each device is limited to
  60 requests per minute.
- The dashboard listener binds 127.0.0.1 on a second port and is never part
  of the IIS rule, so the internet cannot reach it. An optional password
  covers proxying it later.
- `clipture-telemetry-server set-url | issue <friend> | revoke <friend> |
  devices | stats | delete-device | query-key | serve`.

### Query API

`serve --query <address>` adds a read-only JSON API for the admin's other PCs:

- It answers loopback, RFC 1918, link-local and IPv6 unique-local peers only
  (checked on the TCP peer address, so it must never sit behind IIS or any
  proxy), and needs `Authorization: Bearer <key>` from `query-key`; the server
  stores only the key's hash.
- It reads through a separate read-only SQLite connection (`query_only`), so
  it can neither change data nor hold the ingest connection.
- `GET /api/v1/devices`, `/devices/<id>/days`, `/devices/<id>/sessions`,
  `/sessions/<device>/<session>` (summary, minutes, events), `/events`
  (filter by friend, device, level, code, `days` or `since_ms`), `/compare`
  (by version, GPU, driver, backend or friend), and `POST /api/v1/sql` for one
  read-only statement with parameters: at most 5,000 rows and 5 seconds.
- Answers are sized for reading, by a person or an agent: `GET /api/v1/digest`
  is a plain-text summary of every device, version, GPU, the worst sessions
  and the latest incidents; `/versions` puts the newest release first with
  every rate per recorded hour, then compares each device on its newest
  version with the one before (same PC, so only Clipture changed) and marks
  it worse, better or the same; `/incidents/<id>` summarizes each metric that
  moved (before, at the trigger, after, last) and lists the unchanged ones on
  one line; `/sessions/...` has one short row per minute; `?format=tsv` turns
  any JSON table into tab-separated text.
- The server PC's firewall rule for the port is limited to `LocalSubnet`.

SQLite (WAL) schema:

| table | key | contents | kept |
| --- | --- | --- | --- |
| `friends` | id | name, created, revoked | forever |
| `invites` | sha256(token) | friend, created, expires, used, device | forever |
| `devices` | id | friend, public key, enrolled, last seen, `last_seq`, latest install summary, revoked | forever |
| `raw_batches` | id | device, received, seq range, gzip body | 30 days, so records can be re-parsed after a schema change |
| `runs` | device + run | started, ended, end reason, version, install and settings summary | forever |
| `sessions` | device + session | run, start, end, minutes, dropped frames, saves, failed saves, warnings, errors, worst pressure, backend, target fps | forever |
| `minutes` | device + time | session, fps mean/min, dropped, reasons, the rest of the minute record | 90 days |
| `incidents` | id | device, run, session, time, triggers, lowest recorded fps after the trigger, dropped, the samples | 90 days |
| `events` | id | device, run, session, time, level, code, message, data, count | info 90 days, warn/error 1 year |
| `days` | device + day | recorded minutes, dropped frames, saves, failed saves, warnings, errors, crashes, versions seen | forever |
| `gaps` | id | device, lost seq range, reason | forever |

A batch is applied in one transaction: records with `seq <= devices.last_seq`
are skipped (duplicates and replays), a jump in `seq` adds a `gaps` row, then
`last_seq` advances. Rollups in `sessions` and `days` are updated in the same
transaction, so the dashboard never re-scans minutes for its overview.

### Dashboard

Server-rendered pages with inline SVG charts; no build step, no external
scripts.

1. **Friends**: one row per device: friend, last seen, recorded hours this
   week, dropped frames per recorded minute over 14 days as a sparkline,
   errors in the last 24 hours, and a status: *failing* (an error in the last
   24 hours), *degraded* (over one dropped frame per recorded minute, or
   warnings), *healthy*, or *idle* (nothing for 3 days).
2. **Device**: 14 day lanes, each session a bar along its hours coloured by
   drop rate; version, driver and settings changes as ticks; that device's
   recent events.
3. **Session**: fps and dropped frames per minute, drops stacked by reason,
   events as markers on the same time axis, its incidents; install and
   settings that applied. **Incident**: the same at 2-second resolution plus
   frame gaps and VRAM against its budget, and every sample as a table.
4. **Problems**: every warn/error event, newest first, filterable by friend,
   level and code, each linking to its session.
5. **Compare**: dropped frames per recorded minute over 30 days grouped by
   version, GPU, GPU driver or capture backend.

## Consequences

- One build ships to everyone; friends get telemetry fixes through normal
  updates.
- New Rust domain `src-tauri/src/telemetry/` (enrollment, recorder, journal,
  uploader, scrubbing). A separate, additive `TelemetryApi` like sharing's
  (`src/shared/telemetry.ts`, `src/renderer/platform/telemetry-client.ts`,
  `scripts/migration/fixtures/telemetry-contract.v1.json` and its test); the
  Electron host reports it unsupported.
- The allowlist is the privacy boundary: a new field means changing this ADR,
  the record type and its test. Engine detail keys reach a record only
  through a named mapping, never by copying the diagnostics map.
- The server is reachable from the internet through IIS for ingest only.
- Public users see a Telemetry panel in Diagnostics that does nothing
  without a code.
