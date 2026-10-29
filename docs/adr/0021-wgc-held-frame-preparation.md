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
2. The encoder flushes the held frame itself. When it wakes for a tick it
   calls `FrameQueue::beforeSelect`, which the WGC backend hooks to prepare and
   queue the held frame, then selects. The flush runs before the encoder
   advances its cadence: while the tick is still announced, `push()` keeps
   every frame that tick may select.
3. Frames are held only while that hook is installed and a deadline is
   announced; otherwise they are prepared at arrival.
4. Source switches that recreate a pool, stop and the game source closing drop
   the held frame.

Selection is unchanged: the frame is queued by the thread that consumes it,
before it selects. `CatchUpSelectionTests` drives the hold and a queue that
prepares every frame through the encoder loop over 4,000 jittered ticks at
30-240 FPS, with 40 ms stalls and skips past catch-up, and requires identical
selections; it prepares about 1.2 frames per tick.

## Rejected: a flush thread at a lead window

The first implementation flushed from a separate MMCSS thread 2 ms before
each tick, mirroring ADR 0015. It passed the stress window, but a real CS2
clip recorded with it repeated 13-26 of every 60 frames for four seconds in a
smoke while the game ran at 131-165 FPS. Any delay of that thread past the
tick leaves the tick without its frame. Fault injection reproduced it: with the
flush 20 ms late for 3 s of every 6 s, a run had 202 stutter frames and 17%
fewer fresh frames.

Variants measured under the same faults (flush 8 and 20 ms late, every frame
preparation 6 ms slower, encoder wakes 40 ms late):

- Pause holding for 5 s after a late held frame: 3-5 stutter frames per run,
  because the first late frame of each episode is already lost.
- Safety net, the first frame after each tick prepared at arrival: 0-2
  stutter frames, but 26-40% of frames prepared against 32-57% for preparing
  everything, keeping only a fraction of the saving.
- Wider lead windows: the same dependency on a timer, at a lower saving.

Flushing from the encoder removes the second thread, so nothing can be late
for a tick that 1.7.7 would have served.

## Evidence

Stress window, ADR 0015 setup (`--as-game --compute-load --render-size
1920x1440`, 12 games-priority CPU threads, game and daily audio), 60 FPS
capture, HDR 2560x1440 at 210 Hz, RTX 3080, with load stepping 2x to 16x and
the faults above. Per run (about 1,560 frames):

| | Prepare every frame | Encoder flush |
| --- | ---: | ---: |
| Stutter, no fault / slow preparation / late encoder | 1 / 2 / 0 | 1 / 0 / 0 |
| Prepared / delivered frames | 41-57% | 19-24% |
| Heavy phases (game window 45-63 FPS), missed frames, 3 runs | 100, 66, 29 | 40, 67, 35 |
| Heavy phases, prepared frames | 29-37% | 18-21% |

Engine CPU is unchanged at 0.28-0.36 cores. The encoder now starts the copy
of its own frame; with every preparation slowed by 6 ms its lag p99 stayed
within the range of preparing every frame. Window cost could not be compared
reliably: most runs lost the foreground to the developer's own work.

## Limits

The stress window does not reproduce CS2; confirm on a smoke-heavy round. The
encoder waits for the capture callback lock when a preparation is in
progress. The saving shrinks as the source rate approaches the recording
rate.
