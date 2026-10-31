# ADR 0028: A track for every app playing sound

Date: 2026-10-30

Status: implemented.

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
