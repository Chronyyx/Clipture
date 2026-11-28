# ADR 0028: A track for every app playing sound

Date: 2026-10-30

Status: implemented in 1.8.6; the save-time audio layout fixed in 1.8.8 (below).

## Context

With system audio removed (ADR 0027), the "Detected game/app" source recorded
the detected game plus only the focused non-game app, and the latter was routed
into the old system track (`system-loopback-pcm`, named "System audio" in
clips). Sound from apps in the background, such as music while playing, was
lost unless the player added an explicit app track.

Windows' process loopback (`PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`)
captures one process tree per stream. The exclude mode takes a single target
too, so "everything except the game and these apps" is not one stream; Discord's
"everything except Discord" style capture yields one mix, not tracks.

Windows reports which processes hold an active audio session. On the
development PC at idle that was Chrome, Discord and the engine itself, while
Spotify, Steam and others were inactive; capturing only active sessions keeps
the number of streams small.

## Decision

- The engine gives every process with an active audio session (on any output
  device) its own `app-pid:` capture and track, in the detection loop that
  already finds games. It is not a setting: hosts always send
  `captureGameAudio: true` and no explicit `appAudioProcesses`; the settings
  page shows "Game and apps" as automatic, and only the microphone is a
  choice.
- Manual per-app tracks, the "Add source" flow and the one-time default
  browser/Discord tracks are removed; profiles that carry app or system sources
  are normalized to the microphone plus the always-on game source.
- Excluded: the engine itself, Clipture's processes, Windows system processes
  and anything inside a detected game's process tree (the game has its own
  track, named after the game). One capture per process tree
  (`collapseProcessTreeRoots`) and one track per app name.
- A capture outlives its session going inactive by 30 s, so pausing playback
  does not restart it. Footage already captured stays in the replay buffer and
  is grouped by its track at save, even after the capture stops.
- The focused-app route into the system track remains only behind the legacy
  `captureForegroundSystemAudio` flag, which hosts no longer send.
- Game and foreground detection stay: they no longer decide what is recorded,
  but they still name the game's track, file clips under their game, record
  the clip's focused apps and drive the administrator-game notice.

## Consequences

- Clips carry the game, the microphone and one track per other app that played
  sound, each mutable in the editor; there is no way to exclude an app before
  recording. Silent tracks are dropped at save.
- Anything that plays sound gets a track, notification sounds included.
- Captures run only for apps holding an active stream. Apps that keep a stream
  open while silent (voice chat, some games) keep one capture running.
- Game detection is unchanged: a program it classifies as a game (for example
  Wallpaper Engine as a Steam title) gets a game track rather than an app track.

## Correction (1.8.8)

The host's save processor still grouped every `app:` track not chosen as an
app source in settings into one "System audio" track. With app sources gone
from settings, every saved clip from 1.8.6 and 1.8.7 had its apps merged and
its tracks reordered, which re-muxed the whole file with FFmpeg after each
save (1-4 s for a 2-minute 1440p clip, and a full read and write of it). The
save processor now keeps every recorded track, in the engine's order, and
only names them; a save no longer runs FFmpeg for audio. Clips already saved
keep their merged track.


## Correction (2026-11-07): sounding processes, on notification

The game's track captured the top of the game's process tree, on the
assumption that tree loopback reaches every descendant. It does not reach
through every launcher level: Fortnite runs bootstrapper, launcher, an
anti-cheat wrapper and the client, and a capture of the bootstrapper or
launcher is silent while the client plays. The client itself is hidden from
path queries by anti-cheat, so it was never a game candidate, and as a process
inside the game it got no app track either. Its sound was lost.

- The game track now captures the processes inside the game's tree that hold
  a sounding session (collapsed per tree), not the tree's top. A game that has
  not played sound has no capture yet. Sounding processes stay bound for the
  same 30 s grace as app tracks.
- `AudioSessionWatcher` keeps the set of sounding processes from Windows'
  session-created, session-state and device notifications, with a 1 s rescan
  as backup. A change wakes the detection loop for an audio-only pass at once;
  foreground, GPU priority and device-removal checks keep their 2 s cadence,
  since GPU priority counts passes. A new sound gets its capture in tens of
  milliseconds instead of up to the next pass.

## Cost of the detection pass (2026-11-10)

The audio pass opened every process and matched its path against every
installed game on every pass, 60-73 ms each, and the notifications above made
the pass more frequent. ADR 0032 changes that: a process is asked once whether
it is a game, the process list is taken once per full pass and reused by
audio-only passes, and the watcher's rescan asks the sessions it already
holds, listing all sessions again on a session notification and on every
third rescan.

## Correction (2026-11-28): every sounding root of an app

"One track per app name" was enforced by capturing only the first process
tree root of each name and dropping the rest. Discord plays voice from its
renderer and join/leave sounds from its Chromium audio service, sibling
processes under a main process that holds no session, so the clip kept the
join sounds (the lower PID) and lost the call. Every sounding root now gets a
capture (`appAudioCaptureSpecs`); roots of the same name still share one
track, because the source id carries no PID and the coordinator mixes them.
