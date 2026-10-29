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

The remaining repeats, about 1.5 per second in every run whatever the input
path, are intervals in which Windows delivered no game-window frame at all
(187 of 190 in the BGRA block; 3 had frames whose timestamps were older than
the interval). They are outside the capture pipeline and not addressed here.

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
One live match was measured; confirm across more sessions. Running a second
engine during a match caused visible hitches for the player: live probes are
for investigation only.
