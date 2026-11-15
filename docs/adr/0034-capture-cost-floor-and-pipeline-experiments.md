# ADR 0034: What capture costs the game, and pipeline experiments that did not pay

Date: 2026-11-14

Status: findings; no default changed. Kept for the next attempt at the
pipeline's share of the cost.

## Setup

Stress window, CS2 setup of the frame-stability skill (`--as-game --reflex
--compute-load --render-size 1920x1440 --cpu-games-priority --cpu-threads 12`,
daily audio), RTX 3080, Windows HDR on, 2560x1440 at 210 Hz, recording 60 fps
P3 unless stated. Clipture and Wallpaper Engine closed. "Cost" is window fps
lost against the window alone in the same run, per phase (smoke / molly /
spike). Mean ± standard deviation over runs; runs where the window lost focus
are excluded. The same build varies by 2-4 points between runs, so changes
under that are not claimed.

## Capture forces composition; half the cost is the floor

The window's presentation mode is read per frame
(`IDXGISwapChainMedia::GetFrameStatisticsMedia`). Alone it presents through
hardware independent flip (overlay) in every frame. Under every capture the OS
offers it is composed in every frame.

| Capture | Runs | Cost smoke / molly / spike | DWM 3D | Engine 3D |
| --- | ---: | --- | ---: | ---: |
| Bare WGC window session, frames discarded (`clipture_capture_floor --fp16 --interval-ms 8`) | 3 | 4.8 ± 0.2 / 2.8 / 2.4 % | | |
| Clipture, hybrid WGC (default) | 2 | 9.7 / 7.3 / 4.3 % | 0.17 % | 1.13 % |
| Clipture, WGC game window only | 3 | 9.7 / 7.1 / 6.0 % | 0.23 % | 1.10 % |
| Clipture, WGC monitor only | 3 | 11.3 / 6.7 / 6.0 % | 0.25 % | 1.06 % |
| Clipture, DXGI Desktop Duplication | 3 | 13.5 / 8.1 / 5.7 % | 1.16 % | 1.72 % |

- A WGC session that does nothing costs about half of what Clipture costs.
  That half is the price of leaving independent flip, paid by any capture
  outside the game. `docs/native-game-capture-evaluation.md` weighs capturing
  inside the game instead and does not adopt it.
- Hybrid capture costs the same as game-window-only capture: the idle monitor
  session with its undrained pool (ADR 0023) is free. Throttling it
  (`perf/idle-source-throttle`, `CLIPTURE_IDLE_SOURCE_INTERVAL_MS`) has nothing
  measurable to save and was not benchmarked further.
- Desktop Duplication recorded with no missed frames here but cost the game
  about 4 points more in smoke, with seven times DWM's GPU time.
- Clipture's own work is about 1.1 % of the 3D engine, yet the pipeline adds
  about 5 points on top of the floor. The loss is out of proportion to the GPU
  time, which points at preemption and queue bubbles in a game that keeps its
  queue short (Reflex), not at the work itself.

## Pipeline experiments

| Change | Runs | Cost smoke / molly / spike | Recording | Verdict |
| --- | ---: | --- | --- | --- |
| Default (integrated) | 6 | 12.2 ± 2.6 / 6.3 / 3.1 % | missed 21, lag p99 48 ms | |
| NVENC P1 instead of P3 | 6 | 12.2 ± 3.5 / 5.9 / 3.7 % | missed 6.5, lag p99 37 ms | No game gain; see below |
| Shared capture/NVENC device (`CLIPTURE_ISOLATE_NVENC=0`) | 3 | 9.9 ± 2.4 / 7.6 / 5.1 % | missed 22 | No difference (as ADR 0029 found at realtime) |
| No audio capture | 3 | 10.2 ± 2.2 / 6.8 / 3.2 % | missed 4 | No difference in cost |
| Direct slot encode, normal class (`perf/direct-slot-encode`) | 3 vs 3 | 13.0 vs 14.1 / 7.7 vs 8.0 / 9.1 vs 6.7 % | unchanged | Within noise |
| Direct slot encode, realtime class | 6 vs 6 | 12.2 vs 13.1 / 7.4 vs 7.9 / 6.0 vs 7.7 % | missed 32 vs 20, lag p99 53.5 vs 49.6 ms | Rejected: recording worse |

Direct slot encode removes the encoder device's per-frame copy (NVENC reads
the shared capture slot after a CPU wait on its ready fence, submission depth
1). The copy it removes is about 0.02 % of the 3D engine; the slots held
through each encode and the CPU-side wait cost recording more than the copy
cost the game. This rules out that implementation, not other ways of feeding
NVENC.

### NVENC P1

Real clips re-encoded at Clipture's rate control (low-latency tuning, VBR CQ
21, 40 Mbps cap, no B-frames, lookahead or AQ), scored against the source:

| Clip | Preset | Bitrate | VMAF | PSNR |
| --- | --- | ---: | ---: | ---: |
| CS2, 20 s | P1 | 39.4 Mbps | 82.15 | 32.75 dB |
| CS2, 20 s | P3 | 39.3 Mbps | 82.22 | 32.75 dB |
| Deadlock, 20 s | P1 | 12.2 Mbps | 87.93 | 35.76 dB |
| Deadlock, 20 s | P3 | 12.2 Mbps | 87.97 | 35.76 dB |

On this GPU P1 is the same quality at the same bitrate and delivers frames to
the recording about 10 ms sooner. It does not change the game's cost. The
preset is a user setting; it is left as chosen.

## Saving a clip mid-game

Two saves per run, in the smoke and molly phases (`--save-at 7,20`). Game
frame times in the 3 s after each save against the 3 s before:

| Engine | Save time | p99 change | Worst frame change |
| --- | ---: | --- | --- |
| Baseline (2 runs) | 68-81 ms | +0.01 to +0.14 ms | up to +0.7 ms |
| Integrated (2 runs) | 60-72 ms | -0.02 to +0.16 ms | up to +0.2 ms |

A save does not visibly disturb the game.

## 120 fps

One valid pair: cost 15.3 / 10.3 / 7.3 % (baseline) and 15.3 / 10.6 / 7.6 %
(integrated); engine CPU 0.22 and 0.11 cores. About 3 to 5 points above 60 fps,
as ADR 0029 found the cost is per recorded frame.

## Encoder input format and the HDR tone-map pass

On an HDR desktop an SDR game window is captured in FP16 and tone-mapped
every recorded frame. Three inputs, stress window, 3 runs each, alternated:

| Input | Cost smoke / molly / spike | Missed | Lag p99 |
| --- | --- | ---: | ---: |
| BGRA, tone-mapped FP16 capture (default) | 8.5 ± 2.2 / 5.3 / 5.4 % | 27 | 52.2 ms |
| NV12 via the video processor (`CLIPTURE_NV12_INPUT=1`) | 9.2 ± 1.0 / 6.3 / 6.3 % | 33 | 54.6 ms |
| 8-bit game-window capture, no tone map (`CLIPTURE_SDR_GAME_BGRA=1`) | 7.8 ± 1.7 / 5.1 / 3.4 % | 26 | 51.3 ms |

Live in Fortnite (player moving, so scenes differ between blocks), NV12 against
BGRA in three ABBA pairs: 93.8 / 95.6 / 89.6 against 99.0 / 99.4 / 95.7 fps;
one NV12 block had 102 frame spikes a minute against 13-20 in the others.

- NV12 input costs about a point more in the window, in line with the sign of
  the Fortnite pairs. Replacing NVENC's own RGB conversion with a separate
  conversion pass does not pay; NVENC's conversion is not the hidden cost.
- 8-bit game capture is lower in all three phases but inside the spread. It
  stays experimental: it clips HDR games' highlights.
- Engine VRAM disagreed between measurements: 584 against 677-707 MB in
  Fortnite (NV12 lower), 543 against 528 MB in the window. The per-process
  counter counts shared textures per opening device (ADR 0023); no VRAM claim
  is made for either input.

A tone map, NV12 conversion or copy pass changes the window's cost by about a
point or less each. What remains of Clipture's share is not in the per-frame
passes measured here.

## Not measured

HDR games (`--hdr`), VRAM pressure (`--vram-mb`), and the GPU raise policy of
ADR 0033 under starvation (`--max-latency 3`) were scheduled and not run. HAGS,
VRR, fullscreen optimizations and frame generation need system setting
changes and were not tested. No GPUView or Nsight trace was taken.

## Next

The pipeline's share on top of the floor did not respond to one submission
instead of two (shared device), to removing the encoder copy, to NV12 input or
to skipping the tone map. Measuring it further needs GPU-side timing of the
game's own work (GPUView or Nsight) rather than window frame rates.
