# ADR 0022: BGRA encoder input; no NV12 conversion pass on the game's engine

Date: 2026-10-29

Status: implemented; default. `CLIPTURE_NV12_INPUT=1` restores the NV12
conversion for A/B runs.

## Context

Real CS2 clips repeated 10-25 of every 60 frames for seconds at a time, in
smokes and in ordinary play, while the game ran at 150-200 FPS. The stress
window never reproduced it, before or after ADRs 0015-0021, and it was long
attributed to VRAM or GPU pressure. Freeing VRAM (ADR 0018) and halving capture
work (ADR 0021) did not change it.

A selection trace with capture events (`CLIPTURE_SELECTION_TRACE_PATH`, now
recording every WGC arrival, recorded-source switch and late tick;
`scripts/performance/analyze-selection-trace.cjs` explains each repeated tick)
was taken with a headless engine during a live CS2 match:

- Windows delivered 150-210 game-window frames per second throughout.
- 1,336 of 2,081 repeated ticks had a frame for their interval that arrived in
  time and was never queued; most of the rest were stretches where the desktop
  was recorded because the game was not foreground.
- The engine's `captureSlotDrops` was 2,036. `CaptureTexturePool::acquire`
  skips a frame when every free slot still waits for the encoder's GPU read
  (`gpuPending`). The encoder keeps up to four frames in flight and the pool
  has four warm slots.

Letting the pool grow (up to 12 slots) did not help: it filled to 12 and still
dropped 1,100 frames in three minutes, against 987 without growth. The
encoder's GPU reads were stalled, not slightly late.

With NV12 input each new frame is converted by a `VideoProcessorBlt` pass on
the 3D engine, which CS2 keeps saturated (NVIDIA Reflex On + Boost, uncapped
frame rate), and the capture slot is held until that pass completes. With BGRA
input the encoder copies the slot into its own BGRA surface (`CopyResource`)
and NVENC converts colour itself.

## Decision

`nv12Input` defaults to off: native-size frames use persistent per-slot BGRA
encoder input surfaces. Raising the encoder's GPU priority was rejected: it
takes GPU time from the game to protect the clip.

## Evidence

Same CS2 match, alternated two-minute blocks, headless engine beside the
developer's own Clipture, HDR 2560x1440 at 210 Hz, RTX 3080:

| | NV12 conversion | BGRA input |
| --- | ---: | ---: |
| Capture slot drops | 1,352 | 1 |
| Distinct output FPS | 51.2 | 58.2 |

Repeats the trace attributes to a frame that arrived in time but was never
queued follow the engine's slot drops run by run (all while recording the game
window): 1,227 with 2,036 drops over four minutes; 761 with 1,100 (pool
growth); 590 with 987; 884 with 1,359 (NV12 block); 23 with 1 (BGRA block); 6
with 1 (lobby). A drop repeats a tick only when no other frame for that tick
was queued, so the repeat count stays below the drop count.

A second match, BGRA input, 102 s recording the game window: 1 slot drop, 19
in-time-but-unqueued repeats, 1.6 repeats per second in all.

The stress window reproduces it once the window runs NVIDIA Reflex low latency
with boost (`--reflex`, as CS2's "On + Boost"); without Reflex it never did.
ADR 0015 setup, 60 FPS, no encoder stall injected:

| Stress window | Reflex | Slot drops | Missed | Longest freeze |
| --- | --- | ---: | ---: | ---: |
| NV12 | on | 7, 31 | 43, 28 | 133, 67 ms |
| BGRA | on | 0, 0 | 2, 1 | 50, 33 ms |
| NV12 | off | 0 | 1 | 33 ms |

The first NV12 run with Reflex lost the foreground; its 58 stutter frames are
weaker evidence than its drops. Lab trials of encoder input changes must use
`--reflex` from now on.

The remaining repeats, about 1.5 per second in every run whatever the input
path, are intervals in which Windows delivered no game-window frame at all
(187 of 190 in the BGRA block; 3 had frames whose timestamps were older than
the interval). The same traces record the hybrid desktop source, which keeps
running while the game window is recorded. In every one of 772 game-window
gaps over 20 ms across three runs, the desktop source kept delivering, at a
median of 80-86 distinct images per second (122-136 overall): CS2 presented
and the monitor showed it, but Windows.Graphics.Capture of the game window
skipped those frames. They are outside the capture pipeline and not addressed
here. Hybrid capture receives those desktop frames and discards them; using the
newest one for a tick that has no game-window frame would fill most of them.

Known colours (pure RGB, grey 128, a skin tone, white) captured through both
paths decode to identical 8-bit values. Desktop, headless, two runs each:
engine CPU 0.27-0.28 cores against 0.25, engine dedicated VRAM 773-803 MB
against 834-891 MB (the conversion stage's textures go away; BGRA encoder
surfaces are larger, 14.1 against 5.3 MiB each).

The 2026-09 desktop matrix that chose NV12 (`docs/cs2-capture-performance-
investigation.md`) measured lower CPU without a game running and noted it did
not rule out stalls under shared GPU contention.

## Limits

Scaled output (a recording resolution below the source) still converts on the
GPU through the video processor and can still stall under a saturated game.
Two live matches and the Reflex stress window were measured. Running a second
engine during a match caused visible hitches for the player: live probes are
for investigation only.
