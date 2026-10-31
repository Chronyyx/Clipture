# ADR 0027: Remove the system audio track

Date: 2026-10-30

Status: implemented; the migration's app-track step is superseded by ADR 0028.

## Context

The system audio source recorded either the whole Windows mix (loopback) or a
chosen set of processes mixed into one track. It was on by default while the
"Detected game/app" track was off, so default clips carried game sound through
the system track. In practice players turned system audio off and used the
per-process tracks instead: the detected game or app, separate app tracks
(browser, Discord) and the microphone. The whole-mix track duplicated those
sounds and also recorded notifications and anything else playing.

Per-process capture (`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`) is what
the game and app tracks use. Some games run as administrator (ADR 0024), so it
was checked against an elevated process: the unelevated engine, given only an
`app-pid:` target for a PowerShell instance started with "Run as
administrator" and playing a looping sound, bound the process tree and saved a
clip whose only audio track measured -24.5 dB mean, -9 dB peak.

## Decision

- The system source is removed from the default sources, the settings UI (its
  row and configuration dialog) and both hosts' configure mapping. The engine
  protocol keeps `includeSystemAudio`, `captureForegroundSystemAudio` and
  `systemAudioProcesses`; hosts always send them off, because the engine
  defaults `includeSystemAudio` to on when the field is absent.
- "Detected game/app" is on by default.
- Profiles with system audio enabled are migrated on load: the game track is
  turned on, and processes picked into the system mix become separate app
  tracks unless one already exists. A disabled system source is dropped
  without changing the game track.

## Consequences

- Clips record only what has a track: the game or foreground app, chosen apps
  and the microphone. Sound from other apps (music players, a second browser,
  Windows notifications) is not recorded unless the player adds an app track.
- No duplicate of game sound in a second track, and no capture of unrelated
  system sounds.
- The engine's system-loopback code remains for the legacy Electron path and
  the protocol; removing it is a separate engine change.
