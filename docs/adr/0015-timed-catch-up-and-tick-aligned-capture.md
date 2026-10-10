# ADR 0015: Timed catch-up, tick-aligned acquisition, High-class capture thread

Date: 2026-10-07

Status: accepted; default. Each part keeps a process-local switch for A/B runs
(`CLIPTURE_TIMED_CATCH_UP`, `CLIPTURE_TICK_ALIGNED_ACQUIRE`,
`CLIPTURE_HIGH_PRIORITY_CAPTURE`, all `0`/`1`).

## Context

CS2 clips recorded with 1.6.4 and later stutter during molotovs, flashes and
smokes while the game itself keeps running at 170-180 FPS. A 60 FPS clip from
2026-10-04 has up to 17 visually identical frames per second in those moments
and none in ordinary movement. The frames are not bit-identical (NVENC
re-encodes the repeated source), so exact-duplicate checks miss them; a
per-frame motion check finds them.

A frame-stamped synthetic workload (`engine/tools/stress-window`) reproduces the
pattern only when the encoder cadence wake is late, which busy games cause and
which the 1.6.4 catch-up was added for. `EncoderCadence` then returns every due
tick back to back. Each tick called `consumeAllAndGetLatest`: the first took the
newest frame and cleared the queue, the rest found nothing and repeated it.
A 40 ms late wake therefore produced two repeats and moved newer footage onto
an earlier timestamp.

Two further costs showed up in the same trials. Desktop Duplication reports
every desktop update, and each was copied into a capture slot (FP16 under HDR)
although only the newest before a tick is encoded: 5,000-5,500 copies for
2,000 encoded frames. Acquisition also ran in MMCSS "Capture" (Medium class),
the class game engines use for their own threads.

## Decision

1. **Timed catch-up.** A tick that is more than 2 ms late, or has further ticks
   due behind it, takes the newest frame at or before its own deadline
   (`FrameQueue::consumeLatestAtOrBefore`, source timestamps in the media clock;
   rule in `CatchUpSelection.hpp`). This includes the last tick of a burst: it
   is still up to one interval late, and taking the newest frame there showed
   footage early and repeated it on the next on-time tick. On-time ticks still
   take the newest frame. If no frame is due the previous frame is held.
2. **Tick-aligned acquisition.** The encoder publishes its next deadline on the
   frame queue (`announceNextDeadline`). DXGI acquisition sleeps until 2 ms
   before it (`TickAlignedAcquire.hpp`), then polls as before until the tick.
   DXGI accumulates updates while nothing is acquired, so the first acquire in
   the window returns the newest image. An unknown or past deadline falls back
   to continuous polling, so a late encoder never starves capture.
3. **High-class capture thread.** Acquisition registers as MMCSS
   "DisplayPostProcessing" (High class), falling back to "Capture" if that task
   is unavailable. The loop needs about 0.5 ms of CPU per frame.

GPU thread priority, queue capacity, slot counts, quality, cadence and audio
are unchanged. `CLIPTURE_RAISED_GPU_PRIORITY=0` exists for A/B only; dropping it
did not reduce game cost measurably.

## Results

Stress trial: stretched 1920x1440 window as `cs2.exe`, compute-shader smoke,
12 MMCSS "Games" CPU threads, game audio playing, daily audio sources, HDR
desktop, 60 FPS capture, `CLIPTURE_TEST_ENCODER_STALL=40,700`. Two runs each.

| Configuration | Stutter frames | Longest smoke / molly freeze | Smoke lag p99 |
| --- | ---: | --- | ---: |
| Previous defaults | 84, 83 | 50 / 50-67 ms | 53-58 ms |
| Timed catch-up | 6, 0 | 17-33 / 17-33 ms | 23-26 ms |
| + tick-aligned acquisition | 2, 2 | 17-33 / 33 ms | 22-24 ms |
| + High-class capture | 1, 3 | 17 / 33 ms | 18-19 ms |
| + late final tick timed (default) | 3 | 17 / 33 ms | 17 ms |

Harder stall patterns, same setup:

| Stall | Previous defaults | Default |
| --- | --- | --- |
| 65 ms every 0.7 s | 152 stutter frames, 83 ms freezes | 0, longest freeze 17 ms |
| 25 ms every 0.2 s | not run | 0, longest freeze 17 ms |
| 65 ms every 0.7 s at 120 FPS | 63 (40 ms stalls) | 1 |

Without injected stalls the defaults record 0 stutter frames. An HDR (scRGB)
window with 40 ms stalls records 2.

Interleaved game-cost bench at 60 FPS (window alone vs recorded): smoke 16.3% to
11.9%, molly 10.6% to 8.6%.

## Limits

The stress window reproduces the mechanism, not CS2 itself: Reflex, CS2's
presentation and its exact thread priorities are not modelled, and the encoder
stall is injected. Validate a release candidate on a CS2 demo of a smoke-heavy
round.
