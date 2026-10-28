# ADR 0021: Prepare only the WGC frames an encoder tick can select

Date: 2026-10-28

Status: implemented; default since 1.8.0. `CLIPTURE_HOLD_UNTIL_TICK=0` restores
preparing every frame for A/B runs.

## Context

Windows.Graphics.Capture delivers every compositor frame. On a 210 Hz HDR
monitor recorded at 60 FPS that is about 3.5 frames per encoded frame, and
since ADR 0018 each one is copied into a capture slot and tone-mapped at
arrival. ADR 0018's early release frees a superseded frame's slot when the
next frame is queued, but its GPU copy and tone-map pass have already run on
the GPU the game is using. ADR 0015 removed the same waste from Desktop
Duplication by acquiring only in a 2 ms window before each tick; WGC pushes
frames instead, so that approach does not apply as is.

## Decision

The WGC callback holds the newest recorded-source frame unprepared
(`HeldFrame.hpp`), keeping one of the pool's buffers checked out:

1. When a newer frame arrives, the held one is closed unprepared if
   `FrameQueue::wouldSupersede` says no remaining tick could select it, the
   rule `push()` uses for ADR 0018's early release. Otherwise it is prepared
   and queued first.
2. A frame that arrives inside the next tick's 2 ms lead window, past a late
   deadline, or with no announced cadence is prepared at once, as before.
3. A flush thread (MMCSS, like DXGI acquisition) prepares the held frame when
   the lead window opens.
4. Source switches that recreate a pool, stop and the game source closing drop
   the held frame.

Selection is unchanged: a frame any tick could choose is still prepared and
queued before that tick. `CatchUpSelectionTests` drives the hold and a queue
that prepares every frame through the encoder loop over 4,000 jittered ticks
at 30-240 FPS, with 40 ms stalls and skips past catch-up, and requires
identical selections.

## Evidence

Stress window, ADR 0015 setup (`--as-game --compute-load --render-size
1920x1440`, 12 games-priority CPU threads, game and daily audio,
`CLIPTURE_TEST_ENCODER_STALL=40,700`), 60 FPS capture, HDR 2560x1440 at
210 Hz, RTX 3080. Seven alternated runs; the means below use the four whose
window kept focus (two each).

| Means | Prepare every frame | Hold until tick |
| --- | ---: | ---: |
| Prepared / delivered frames | 35% | 18% |
| Engine CPU (cores) | 0.33 | 0.29 |
| Stutter / missed frames | 0.5 / 2 | 0.5 / 0.5 |
| Longest smoke freeze | 33, 17 ms | 17, 17 ms |
| Fresh frames, all seven runs | 99.88% | 99.95% |
| Window cost: clear / smoke / molly | 56 / 12.4 / 6.1% | 48 / 9.6 / 5.6% |

Smoke lag p50 fell from 10.2-15.4 ms to 9.1-10.1 ms. Clear-phase lag p99 rose
from about 7.1 to 7.4-8.6 ms: a held frame is prepared 2 ms before its tick
rather than at arrival.

## Limits

Window cost varies between runs about as much as the measured difference;
the GPU work removed is certain, the frame-rate gain needs confirming in a
real game. A flush thread starved past the tick would let that tick take an
older frame; it runs in the MMCSS class DXGI acquisition already relies on.
The saving shrinks as the source rate approaches the recording rate.
