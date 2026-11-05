# ADR 0031: Cap WGC delivery at twice the recording rate, up to 60 fps

Date: 2026-11-06

Status: experimental; default until confirmed in real sessions.
`CLIPTURE_WGC_MIN_UPDATE_100NS=<100 ns>` forces an interval for A/B runs; `0`
restores uncapped delivery.

## Context

ADR 0007 requested a `MinUpdateInterval` of 0 so Windows would deliver every
frame the source presents. Its cost to the game was never measured. With
`holdUntilTick` (ADR 0021) the engine prepares one frame per recorded frame
and closes the rest, so an uncapped game at 1,200 fps had about 16,000 frames
delivered in a 33 s stress run to record 2,000.

ADR 0029's cost study found that realtime's extra cost is per recorded frame
and that no in-process change reduced it (shared device, NV12 input, D3D11
thread priorities). A probe doing only the engine's two per-frame copies
showed those copies cause most of realtime's extra cost in GPU-bound phases,
so moving NVENC to a separate normal-priority process could win back at most
1-2 points.

## Decision

Recording at 60 fps or less, the game-window and monitor WGC sessions request
half the recording frame period as `MinUpdateInterval` (60 fps: 8.33 ms;
30 fps: 16.7 ms). Above 60 fps delivery stays uncapped. The capture run loop
re-applies the interval when the recording rate changes. Old Windows without
`IGraphicsCaptureSession5` keeps its own cadence, as before.

## Evidence

Stress window (`--reflex --compute-load --render-size 1920x1440`, 12 CPU
threads), RTX 3080, engine at `realtime`. Game fps lost against the window
alone; runs that lost window focus are excluded.

| Recording | Delivery | Clear | Smoke | Stutter | Missed | Freshness |
| --- | --- | --- | --- | --- | --- | --- |
| 30 fps | 2x rate | 0.15 / 0.22 % | 6.9 / 6.1 % | 0 / 0 | 0 / 0 | 100 % |
| 30 fps | uncapped | 0.54 / 0.41 % | 10.7 / 10.1 % | 0 / 1 | 0 / 1 | 99.9-100 % |
| 60 fps | 2x rate | 1.42 / 1.66 % | 10.2 / 10.9 % | 0 / 1 | 24 / 19 | 98.8-99.0 % |
| 60 fps | uncapped | 4.75 / 3.79 % | 12.6 / 12.6 % | 0 / 0 | 0 / 0 | 100 % |
| 120 fps | 2x rate | 0.5-2.7 % | 13.1-16.8 % | 18-23 | 29-121 | 96.9-99.3 % |
| 120 fps | uncapped | 1.4-8.8 % | 11.3-16.5 % | 4-12 | 1-11 | 99.7-100 % |

At `normal`, recording 60 fps, the cap took clear from 1.66 to 1.0 % and
smoke from 8.2 to 7.3 %. One delivery per recorded frame (60 fps) cut the
cost further but stuttered (203-219 frames), and is not used.

At 120 fps the cap doubled stutter and saved nothing in the GPU-bound
phases: a game rendering about 180 fps never reaches 240 deliveries a second,
so the cap only applies when the game is far above the recording rate.

With three frames queued (`--max-latency 3`, the starved case of ADR 0029)
the cap changed nothing at `normal` (726-745 stutter frames either way),
`realtime` still removed the stutter (0-2 frames), and the adaptive engine
still raised on queued drops (164-174 stutter frames before the raise).

## Consequences

- Recording at 60 fps at realtime, the game gets back about 2.7 points in
  light phases and 2 in GPU-bound ones; at 30 fps about 4 in GPU-bound ones.
- About 1 % of frames recorded at 60 fps are one source frame older than
  uncapped. Stutter is unchanged.
- Real games present unevenly; the stress window presents evenly. Real
  sessions (diagnostics freshness, clips) decide whether this stays default.
