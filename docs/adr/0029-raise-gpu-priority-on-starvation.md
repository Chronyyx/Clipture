# ADR 0029: Raise GPU priority per game, only when it starves recording

Date: 2026-10-31

Status: implemented; refines ADR 0025.

## Context

ADR 0025 put the engine's GPU work in the `realtime` scheduling class whenever
"Prioritize recording on the GPU" is on. That fixed the freezes on PCs where a
game starves capture and NVENC, but most PCs never had them: an RTX 5070 and
the development RTX 3080 recorded uncapped games smoothly at `normal`, and the
player on the 5070 reported the game running worse with realtime. At `normal`
the 3080 in uncapped Overwatch dropped 1.4 frames a second at 60 fps (about
2 %), which clips do not show.

The stress window (`--max-latency`, frames the window may queue ahead on the
GPU) showed what decides starvation on the 3080, recording at 60 fps:

| Window queue | Adapter 3D busy | Recording at `normal` | At `realtime` |
| --- | --- | --- | --- |
| 1 frame | up to 93 % | clean | clean |
| 3 frames | 68 % | first losses | clean |
| 3 frames | 82-97 % | 17-27 % fresh, freezes to 450 ms | clean |

GPU busy alone does not predict starvation (82 % starved with three frames
queued, 93 % was clean with one). Work packets in flight on the adapter
(`D3DKMTQueryStatistics`, submitted minus completed, all processes) separated
the queue depths at both classes: one frame 5-24 at `normal` and 4-5 at
`realtime`, three frames 83-89 at `normal` and 33 and rising at `realtime`.

In Overwatch on the same PC (overlay showing 97 % GPU), uncapped kept 46-80
packets in flight (spikes to 113) at either class and dropped 167 frames in
51 s at `normal`, the worst seconds on the spikes, and none at `realtime`;
capped at 141 fps it kept 2-11 and dropped none. The adapter busy counter read
only 44-68 % uncapped, so it cannot judge load under hardware scheduling. CS2
on that PC is CPU-bound (busy about 50 %, packets 3-9) and dropped nothing at
any setting.

Recorded at `normal`, that uncapped Overwatch lost 0-42 frames per 2 s window,
3 or more in 11 of 15 windows, and the clips showed micro-stutters the player
judged unacceptable: recording is the smaller program and must not visibly
stutter. Packets in flight divided by packets completed per second (the queued work
in time, which would scale with GPU speed) did not separate the cases: about
10 ms queued was clean with one frame queued and starved with three. The count
of packets did. A high count alone says a game is queuing ahead, not that
recording suffers, so a raise needs a dropped frame as well.

Once realtime is applied the drops it prevents are gone, so drops cannot tell
a raised engine that it may step down, and stepping down to see would put
stutter into clips.

## Decision

- With the setting on, the engine starts at `normal` and checks the game in
  front once per detection pass (about every 2 s). It raises itself to
  `realtime` on either rule (`GpuStarvationDetector`):
  - Queued drops (fast): 40 or more packets in flight and at least one
    dropped video frame (capture slots, frame-queue overflow, scheduler skips,
    encoder backpressure) together in 2 of the last 3 passes, so within 4-6 s.
    The queue says a game is queuing ahead; the drop confirms this GPU cannot
    keep recording fed at that depth, so the rule scales with hardware
    without calibration: a GPU that copes with deep queues never drops and is
    never raised. One hiccup cannot raise it.
  - Steady drops (slow, for loss without a deep queue): in 10 of the last 15
    passes (about 20 of 30 s) at least 2.5 % of the frames due, and at least
    3, were dropped. A stutter of several seconds or a loading screen does not
    raise it.
  Drops while no game is in front are not counted.
- A raised game is remembered once 40 or more packets have stayed in flight
  for 2 minutes in total in that session (`GpuRememberWatch`). A cutscene or
  one heavy moment ends before that and is not remembered; gameplay that
  needs realtime keeps the queue deep. The memory is `gpu-priority-games.txt`
  in the data folder (path sent as `gpuPriorityMemoryPath`): a random
  per-install key and, per game, an HMAC-SHA256 of the lowercase executable
  name and the GPU's hardware IDs. No name is stored; a lookup hashes the
  running game. A remembered game starts raised every launch on the same GPU;
  a new GPU starts fresh.
- The engine returns to `normal` when the raised game closes, or when fewer
  than 20 packets stay in flight for 60 s (`GpuQuietWatch`; mean of 25 reads
  4 ms apart per pass), as when the player caps the frame rate. That happens
  once per game session: after it, the session is not raised from memory, and
  if drops raise it again, realtime itself shortened the queue and it stays
  raised until the game closes, so stutter cannot cycle into clips. It never
  steps down just to test.
- Turning the setting off, or a hang recovery (ADR 0025), returns to `normal`.
  The host watchdog reads the class on every poll, so its 3 s threshold
  follows a raise.
- Diagnostics report `gpuPriorityRaised`, `gpuPriorityReason` (`queued
  drops`, `steady drops`, `remembered`, `start raised`), `gpuPriorityRememberedGames`,
  `gpuStarvationLossyWindows`, `gpuBusyPercent` and `gpuPacketsInFlight`.
- `CLIPTURE_GPU_SCHEDULING_CLASS` still fixes the class for engineering A/B.
  `CLIPTURE_GPU_PRIORITY_START_RAISED=1` starts raised, for the capture smoke
  test's hang step.

## Consequences

- PCs that record smoothly at `normal` never pay for realtime. A PC whose
  clips only micro-stutter is raised; on one where the game visibly loses
  frames to realtime (reported on an RTX 5070), the player can turn the
  setting off.
- On a starved PC with a deep queue the first session of each game shows
  about two small hitches (4-6 s) before the raise; later sessions start
  raised. Loss without a deep queue takes about 20 s.
- A game that once starved stays raised on that GPU while it keeps the GPU
  busy, even if a later driver would cope at `normal`.
- What realtime costs the game (stress window fps against the window alone,
  3080, recording 60 fps, 2026-11-04): about 1.2 % at `normal` and 2.5-3.5 %
  at `realtime` in light phases, 7-9 % and 12-14 % in the smoke phase. The
  extra cost grows with the recorded frame rate (+0.6, +1.9 and +2.9 points in
  light phases at 30, 60 and 120 fps), so it is per recorded frame. Neither a
  shared capture/NVENC device (one copy and one flush fewer), NV12 input, nor
  D3D11 GPU thread priority 0 (worse, 6-9 %) reduced it. Nor did splitting
  the thread priority (capture +7, encoder -7, or encoder -7 alone): within a
  realtime process every device's work is realtime. The remaining levers are
  time spent raised, or the encoder in a separate process left at `normal`.
- The game in front and the running games came only from the audio detection
  pass, which runs with game or foreground-app audio on, and lists running
  games only with game audio on. With both off the engine never raised; with
  only foreground-app audio a raise stepped down as "game closed" after one
  pass. When game audio is off, the engine now finds the known game in front
  itself and passes every running process name (2026-11-04; experimental
  until confirmed in real sessions). Stress window,
  three frames queued, no audio: 728 stutter frames at `normal` before, raised
  on queued drops and 191-199 after; one frame queued stays at `normal`.
- Thresholds come from one GPU, a synthetic window, Overwatch and CS2. The
  diagnostics fields let exports from starved PCs refine them, or replace the
  drop rule with a prediction.
