# ADR 0032: Keep the engine's background loops cheap

Date: 2026-11-10

Status: experimental; default until confirmed in real sessions.

## Context

The engine's frame path was tuned in ADRs 0015 to 0031. Its background loops
were not: game detection, GPU priority sampling, diagnostics and the audio
supervisors run for as long as the app does, in a high-priority process next
to the game, and nobody had measured them.

Whole-process CPU cannot show them. Capture, encoding and the microphone
thread use about 0.3-0.45 of a core and vary by 0.03 between runs. CPU per
named thread, from cycle counts, can. Measured that way on the 3080 PC with
about 400 processes running:

- The detection pass opened every process, read its path and matched it
  against every installed game: 60-73 ms per pass, every 2 s, and again on
  every audio session change since ADR 0028's notifications.
- Listing the processes through Toolhelp took 17.5 ms, because Toolhelp maps
  its snapshot section again for every entry.
- Reading packets in flight for GPU priority (ADR 0029) took 114 ms of CPU
  per pass: one engine node costs 0.27-0.44 ms to read, the adapter has 14,
  and a pass reads them 25 times. That ran with or without a game, and was
  5-6 % of a core by itself.
- The host's watchdog asked for full diagnostics every second and read six
  fields of the answer.

## Decision

A background loop does work only when its result is used, and asks a question
once when the answer cannot change.

- Whether a process is a game is remembered while the process runs
  (`ProcessVerdictCache`: ID, parent and name; forgotten when the process is
  gone). Roblox is asked on every pass, because it counts only while its
  window is up. A process whose path could not be read is asked again every
  30 s. Installed-game folders are normalized once, when they are scanned.
- The detection loop reads the foreground window once per pass and lists the
  processes once per full pass, with one `NtQuerySystemInformation` call;
  Toolhelp remains the fallback. An audio-only pass reuses the list while it
  names every sounding process.
- The list is published (`ProcessSnapshotFeed`). An app-audio thread whose
  process is not running waits for the next list instead of listing the
  processes itself every 2 s, and sleeps on a condition instead of waking
  ten times a second.
- Packets in flight are read only on a pass that acts on them: boost on, and
  a known game in front or a game raised. After a pass's first sample, nodes
  that were given no work since the previous pass and hold none are left out.
- `gpuBusyPercent` feeds no rule. It is read when diagnostics ask for it
  (`gpuUsage`, on by default); the host's background sampling asks without.
- The host watchdog polls `getProgress`: the counters it compares and the GPU
  class, without building diagnostics. An engine that answers "Unknown engine
  command" is polled with `getDiagnostics` as before. Both are exempt from
  the long-request pause.
- Latency windows are read newest first and only as far back as the horizon,
  without allocating; windows that only count events are counted, not sorted.
- The save writer's pressure sample reads two counters instead of building
  capture statistics before every write. Those statistics take the locks the
  capture threads record under, and the writer may run at background
  priority.
- The WGC backend's control loop polls every 25 ms only while a game window
  is open, for what is above it. Otherwise it sleeps until a new game window,
  a rate change or a failure wakes it, and checks for a removed device once a
  second.
- The audio session watcher's 1 s rescan asks the sessions it already holds.
  It lists every session again on a session-created or session-ended
  notification, on a device change and on every third rescan.

Kept as it was: the host still samples full diagnostics every 2 s. That
sampling feeds the frame-drop history that exports carry, and the engine
still restarts a stopped capture from it.

## Evidence

CPU per thread, ms per second, two 30 s runs each, isolated engine with game
audio on under the host's idle requests (old: diagnostics every second plus
every 2 s; new: `getProgress` every second, diagnostics without GPU usage
every 2 s). Fortnite was in front, so the game path of GPU priority ran.

| Thread | Before | After |
| --- | --- | --- |
| game-detection | 73.9 | 25.6 |
| engine-main (requests) | 0.69 | 0.29 |

Parts, measured separately:

| Work | Before | After |
| --- | --- | --- |
| Game sweep, per pass | 60-73 ms | under 1 ms |
| Whole detection pass in the stress trials | 31-53 ms | 5-6 ms |
| Process list (401 processes) | 17.5 ms | 10.0 ms |
| Node reads in a 5-sample in-flight pass | 70 | 30 |
| Audio session watcher | 1.04-1.13 ms/s | 0.50-0.51 ms/s |
| GPU busy counter, per read | 0.8 ms every pass | on request only |

Stress window, CS2 setup of the frame-stability skill with the injected
encoder stall, engines alternated, window in focus throughout, no dropped
frames in any run. Fortnite was running, so this shows no regression rather
than clean numbers.

| Engine | Stutter | Missed | Worst lag p99 | Engine CPU |
| --- | --- | --- | --- | --- |
| before | 6 / 3 | 4 / 1 | 45.8 / 42.6 ms | 0.33 / 0.30 core |
| after | 7 / 3 | 3 / 0 | 43.0 / 43.2 ms | 0.29 / 0.29 core |

## Consequences

- With a game in front the engine uses about 5 % of a core less; at the
  desktop more, since packets in flight are not read at all. Neither game fps
  nor clip stutter changed measurably: both are set by capture and encoding
  on the GPU.
- A process once seen as a game stays one until it exits, even if its path
  later becomes unreadable (anti-cheat). Before, it stopped counting on the
  next pass.
- A game installed while the engine runs is still not known until the engine
  restarts, as before.
- `gpuPacketsInFlight` is -1 after a pass that did not read them.
  `gpuBusyPercent` is the mean since the previous read that asked for it, and
  -1 on the first read after 10 s without one.
- The `[perf] game_detection` line now times the pass from its start,
  process list included.
- `engine-protocol.v1.json` lists what only the Tauri host sends under
  `tauriCommands`; the contract test checks those against the Rust client and
  the engine, not against the Electron reference.
- `NtQuerySystemInformation` is documented as subject to change. The record
  layout used is the one Toolhelp itself reads, a test compares the two
  lists, and a failed call falls back to Toolhelp.
- Seen while measuring, not changed: an engine with game audio on did not
  exit within 15 s of its input closing (old and new builds), and the
  `audio-mic` thread used about 120 ms of CPU per second with the microphone
  track off in the test configuration.

## Amendment 2026-11-20: the Windows game list, and two waits instead of polls

The installed-game folders were scanned once per engine run, so a game
installed while Clipture ran stayed unknown until a restart (see
Consequences). GTA V from Epic, installed mid-session on the 3080 PC, recorded
with `gameInFront=0` for the rest of it: no game raise, no game audio.

- The engine now also reads the games Windows recognised itself, the Game Bar
  list under `HKCU\System\GameConfigStore\Children`, whatever launcher
  installed them (`platform::WindowsGameList`). Only entries carrying a
  `TitleId`, which Windows matched to its game database, count; entries
  without one are apps marked by hand (Discord, Java runtimes). Windows adds a
  game when it first runs, so it is known by the time it is played.
- The list is read once (about 7 ms for 74 games) and again only when Windows
  signals a change through `RegNotifyChangeKeyValue`. The detection pass
  checks that signal without waiting (about 0.2 us) and, after a reread,
  forgets the per-process verdicts so a running game is asked again.
- Steam and Epic manifests stay as the fallback and for display names
  (`InstalledGamesCatalog`): read on first use, and again only when an unknown
  process runs from inside a launcher library (`\steamapps\common\`, or
  beside an existing Epic install), once per executable. A game installed
  mid-session is then known even where the Windows list is unavailable.
- With no microphone, the `audio-mic` thread no longer wakes every 500 ms. The
  device notification thread already asks it to refresh when a device
  appears; every wake now takes its lock, so the wait has no timeout.
- The UI process startup check waits on a condition for the handshake or the
  process's death instead of checking every 50 ms (ADR 0004).

Not changed: merging the host watchdog's `getProgress` (every second) into
the 2 s diagnostics sample. The two have different lifetimes (the watchdog
lives per engine generation and pauses during long requests; the sampler runs
throughout and reports an engine that never answers), and both together cost
the engine about 0.3 ms of CPU per second (Evidence above).
