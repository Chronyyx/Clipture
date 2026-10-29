# ADR 0023: Engine resource trims after BGRA input

Date: 2026-10-29

Status: implemented. The first six trims below are defaults; each keeps its
switch for A/B runs and fallback. `CLIPTURE_SDR_GAME_BGRA` stays experimental.

Fallback to the earlier pipeline: `CLIPTURE_WGC_DIRECT_TONEMAP=0`,
`CLIPTURE_SKIP_CANONICAL=0`, `CLIPTURE_IDLE_POOL_NO_DRAIN=0`,
`CLIPTURE_WGC_BUFFERS=4`, `CLIPTURE_NVENC_WARM_BITSTREAMS=8`,
`CLIPTURE_NVENC_MAX_REFS=0` (driver default).

## Context

After ADR 0022 the engine was measured part by part to find memory and GPU
time it does not need, without giving up capture stability. Setup: RTX 3080,
2560x1440 at 210 Hz, Windows HDR on, 60 FPS, P3.

`CLIPTURE_VRAM_TRACE=1` logs the process's local and non-local video memory
(`QueryVideoMemoryInfo`) at allocation stages and every 5 s with the encoder's
surface counts. Two things it showed matter for every VRAM comparison:

- The per-process "GPU Process Memory\Dedicated Usage" counter counts a shared
  texture once per device that opens it. The encoder device opens every capture
  slot, so the counter reads about 90 MB above the adapter-wide increase while
  the engine runs (474 against 380-388 MB, headless desktop). Compare the
  adapter-wide "GPU Adapter Memory\Dedicated Usage" delta or private bytes.
- Textures commit on first GPU use, and WGC allocates pool buffers on demand
  up to the pool's count. A pool's buffer count is a ceiling, not an up-front
  allocation; on a quiet desktop one and four buffers cost the same.

Headless desktop, steady state: about 385 MB adapter VRAM and 392 MB private.
NVENC's own allocations, made during the first encodes, were the largest single
item (about 130 MiB local and 46 MiB non-local).

## Measured trims

Headless desktop: two alternated 14 s runs each. Stress window: ADR 0022 setup
(`--reflex`, `--as-game`, compute load, 12 games-priority CPU threads,
`CLIPTURE_TEST_ENCODER_STALL=40,700`) with a non-activating window raised over
the game for 1.5 s every 6 s, so each run has about 15 recorded-source switches.

| Switch | Effect | Stability |
| --- | --- | --- |
| `CLIPTURE_NVENC_MAX_REFS=2` | `maxNumRefFrames` 2 instead of the driver default: -64 MB private, -50 to -75 MB adapter VRAM. Clip size within run-to-run noise over five alternated pairs. | unchanged |
| `CLIPTURE_WGC_DIRECT_TONEMAP=1` | HDR tone mapping reads the WGC surface through a shader view (WGC creates it with `SHADER_RESOURCE`); no FP16 staging texture and no FP16 copy per prepared frame. -30 MB private, -20 to -40 MB adapter. Pixels identical. | unchanged |
| `CLIPTURE_NVENC_WARM_BITSTREAMS=4` | Four warm NVENC output slots instead of eight; more are still allocated on demand. -22 MB private. | unchanged |
| `CLIPTURE_SKIP_CANONICAL=1` | BGRA input copies the capture slot straight into the NVENC input surface; repeats copy again from the slot the scheduler keeps leased. One 14 MiB texture and one full-frame copy per fresh frame less. -15 MB private. | unchanged |
| `CLIPTURE_WGC_BUFFERS=3` | ADR 0019 option 1, now with holding: -50 to -100 MB engine VRAM in game. | 0/0 stutter/missed in both runs |
| `CLIPTURE_IDLE_POOL_NO_DRAIN=1` | The unrecorded hybrid source is not drained, so its pool fills and WGC stops writing into it; a switch drains the stale frames under the callback lock before the source flips. dwm.exe 3D-engine time 3.6-4.1% to 1.2%. VRAM unchanged to +30 MB. Not combined with desktop gap fill, which needs those frames. | unchanged |
| `CLIPTURE_SDR_GAME_BGRA=1` (experimental) | Game window captured as 8-bit BGRA under Windows HDR: -45 to -75 MB engine VRAM, no tone-map pass for game frames. Clips HDR games' highlights. | unchanged |

All seven together against the default, three alternated stress-window rounds:

| | Default | All seven |
| --- | ---: | ---: |
| Stutter / missed frames | 0/1, 1/1, 1/1 | 0/0, 0/1, 0/0 |
| Engine VRAM (process counter) | 834-849 MB | 563-593 MB |
| Engine private memory | 447-449 MB | 331-334 MB |
| dwm.exe 3D-engine utilization | 4.1% | 1.2% |
| Engine 3D-engine utilization | 2.0% | 1.3% |
| Window FPS, smoke / molly / spike | 150-155 / 98-99 / 54 | 158-160 / 101 / 55 |

Headless desktop, all seven: adapter VRAM -92 MB, private 392 to 247 MB.

## Rejected

- **`maxNumRefFrames=1`**: -71 MB private but clips 30% larger at the same CQ.
- **Idle pool swap**: closing the unrecorded source's pool and opening a
  one-buffer pool at each switch (and a full one for the newly recorded
  source). `Close` frees a pool on a quiet desktop and a new session delivers
  its first frame in 3-9 ms, but with the game presenting, engine VRAM grew by
  about 75 MB per switch (789 to 2004 MB in one run) while private memory stayed
  flat. Closed pools are not released while the game runs. With ADR 0019's
  `Recreate` result, pool sizes can only be chosen at first creation.
- **Fewer warm capture slots** (3 or 2) and **NVENC submission depth 2**: no
  measurable saving; slots and surfaces are committed only when used.
- **Timer resolution and the encoder's 0.2 ms spin tail**: no measurable CPU
  difference. Idle engine CPU is 0.04-0.07 cores; no thread dominates.
- **Closing the monitor session while the game is recorded**: the stress window
  presents through independent flip (overlay) with no capture, and is composed
  under any WGC session, window-only and monitor-only alike
  (`IDXGISwapChainMedia::GetFrameStatisticsMedia`). Window capture alone forces
  composition, so nothing is gained.

## Exact SDR and automatic HDR tone mapping

The FP16 path rendered SDR content dark: SDR white as 244, grey 128 as 126, a
200/150/100 skin tone as 194/146/101. The curve rolled luminance off from 75%
of SDR white, which compresses SDR white itself, and encoded with a 2.2 power
curve, while Windows composes SDR content into scRGB with the piecewise sRGB
curve times the SDR white level.

The tone-map pass now re-encodes with the sRGB curve and measures each frame's
peak (group-shared maximum, one atomic per group, copied to a four-deep staging
ring and read without waiting). `ToneCurveControl` keeps the knee at SDR white
while no frame exceeds it, so SDR content maps back exactly; the first frame
above 1.02x SDR white eases the knee to 75% within 150 ms, and 5 s without one
eases it back over 1 s. It runs per frame on the frames already tone-mapped,
for the desktop and the game window alike; no pool changes format.

A WGC recording of the stress window's known patches now decodes to the same
values as the 8-bit capture (grey 128, white 255, 195/147/102 after 4:2:0), and
the stress window's `--hdr` highlights switch the curve to the roll-off.
`clipture_tone_curve` and `clipture_desktop_preparation_pixels` cover the knee
control, all 256 SDR grey levels at 200-nit white, and the switch.

The same measurement cannot make the BGRA game pool automatic: once the pool is
8-bit, HDR that a game turns on mid-session is invisible, and checking for it
needs a second FP16 pool on the window, about half the saving, plus a pool
reopen per format change, the operation that leaked above. The BGRA game pool
stays an experiment.

## Open

- Microphone and system loopback are captured whatever the include settings
  (about 0.02 cores for the microphone thread, and the device stays open).
- A live A/B in CS2 (defaults against the fallback switches, with selection
  traces) is still to do.

## Live check

The developer's own build with these defaults, a CS2 match (HDR 2560x1440 at
210 Hz, Reflex On + Boost, uncapped), measured from counters only, with no
second engine: dwm.exe 3D-engine time 0.6%, engine 1.2%, engine VRAM 757 MB
(process counter), private 540 MB with the daily audio sources. A saved
two-minute clip, checked frame by frame for repeats while the picture moves:
1.3 per second overall, none in the six heaviest smoke seconds. The three
densest seconds were two alt-tabs (desktop recorded at the compositor's rate,
ADR 0017) and a black death transition.
