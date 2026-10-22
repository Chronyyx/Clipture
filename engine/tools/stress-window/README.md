# Stress window

`clipture_stress_window` is a synthetic capture workload for frame-stability
trials. It reproduces the condition behind the CS2 smoke/molotov losses (bursts
of heavy GPU work that delay capture copies and the encoder handoff) without
launching a game. It is built with the engine tests and is never staged or
shipped.

## What it draws

- An uncapped, borderless, top-most flip-model window covering the primary
  monitor, presenting with tearing allowed when supported.
- Scrolling fine detail and a moving bar, so every frame differs and the encoder
  sees game-like motion.
- Load layers: full-screen alpha-blended draws with dependent per-pixel ALU work,
  one draw per layer. The count per phase follows a scripted timeline.
- A frame counter stamp in the top-left corner, drawn last (see
  `FrameStamp.hpp`; `scripts/performance/stress/frame-stamps.cjs` decodes the
  same layout).

`--hdr` presents FP16 scRGB with highlights at 8x SDR white. Whether the engine
runs its HDR tonemapping path depends on the monitor's Windows HDR setting, not
on the window; the trial report records `hdrTonemapping` from engine
diagnostics. CS2 renders SDR, so on an HDR desktop the default 8-bit window
matches it; `--hdr` covers HDR games and Auto HDR.

## Timeline

`--timeline name:seconds:loadMs,...`, default
`clear:4:0,smoke:6:6,clear:3:0,molly:4:10,clear:2:0,spike:1:20,clear:3:0`.
`loadMs` is extra GPU time per frame. A startup calibration measures the GPU
cost of one layer (`layerMs`), so the same timeline produces similar pressure on
different GPUs. Pass `--layer-ms` to pin it; comparisons between engine builds
must use the same value (the trial runner reuses the baseline's).

Without `--autostart` the window renders a `lead` phase until a `start` line on
stdin, then the timeline, then a `tail` phase until `stop` or EOF. Esc quits.
It writes `frames.csv` (counter, phase, present time) and `summary.json`
(per-phase frame rate and frame-time percentiles) to `--out`.

## Matching CS2

A plain GPU-loaded window records cleanly even when CS2 does not. The losses
in real clips cluster on molotovs, flashes and smokes while the game still runs
well above the capture rate, and only these options together reproduce them:

| Option | Why |
| --- | --- |
| `--render-size 1920x1440` | 4:3 stretched borderless, as the CS2 profile here runs; composition scales every frame. |
| `--compute-load` | Load layers as full-screen compute dispatches, like volumetric smoke; coarser to preempt than draws. |
| `--cpu-threads N --cpu-games-priority` | Busy threads registered as MMCSS "Games", outranking normal threads the way game simulation does. |
| `--as-game` (runner) | Runs the window as `cs2.exe`, so game detection and game-audio capture bind to it. |
| `--vram-mb N` | Holds N MiB of VRAM (capped at the budget less 512) and references it every frame, like a VRAM-hungry game. Windows keeps the foreground window resident and demotes the engine instead; a separate process cannot do this. 8600 fills a 10 GB card. |

The runner's `--no-solo` skips the window-alone pass (about a third of each
run) when `windowCost` is not needed; pin `--layer-ms` from an earlier report.
| `--audio` | Soft noise during loaded phases, so the game-audio track is live like grenade sound. |
| `--daily-audio` (runner) | The engine captures audio with the sources from the daily settings. |

The window also takes the foreground (games always hold it) and records the
share of frames it held it per phase (`foregroundPct`). Treat phases far below
100% as invalid.

`CLIPTURE_TEST_ENCODER_STALL=<ms>,<periodMs>` (engine environment, via
`--env`) delays one encoder cadence wake by `<ms>` every `<periodMs>`. Busy
games delay that wake in practice; this injects it deterministically.

## Running a trial

```powershell
npm run build:engine
node scripts/performance/run-stress-trial.cjs --label baseline
node scripts/performance/run-stress-trial.cjs --label candidate `
  --engine build\engine\Release\clipture_engine.candidate.exe `
  --baseline .cache\stress-trials\<baseline folder>\report.json
```

The runner records the window alone, then again while an isolated engine
(`probe-engine-cadence.cjs`: temporary APPDATA, no audio, primary monitor)
captures it, saves the timeline as a clip, and decodes every stamp. Close
Clipture first; a second engine competes for the GPU and the runner warns about
it. Results go to `.cache/stress-trials/<time>-<label>/report.json`; the clip is
deleted after scoring.

## Reading the report

| Field | Meaning |
| --- | --- |
| `stutterFrames` | Repeated output frames in phases where the window (and the display refresh) outran the capture rate by 50% or more. A new frame always existed, so each is a visible hitch. The main number to drive down. |
| `missedFrames` | Repeats whose successor frame had been out longer than the usual path to the recording (baseline lag + one window frame + one refresh). Excludes frames taken early by encoder catch-up, so it undercounts stutter. |
| `freshnessPct` | Unique captured images / min(output frames, frames presented over the same span). |
| `longestFreezeMs` | Longest run of identical output frames. |
| `lagMs` | Clip time minus present time, shifted so the fastest frame is 0. The p99/max tail shows images reaching the recording late. Present time is CPU submission, so heavy phases include the window's own GPU queue; compare lag between builds, not as an absolute latency. |
| `slotPools` | Capture texture and NVENC output slots: `held` at the end, `peak` held at once during the run, and `idleReleases` freed for sitting idle. A peak above the warm set (4 capture, 8 NVENC) means a burst grew the pool; releases mean it shrank back. `slotTimeline` has the slots held each second as `[seconds, capture, NVENC]`. |
| `windowCost` | Window frame rate with capture versus alone, per phase. A capture change that lowers `missedFrames` by raising this is moving the cost onto the game. Ignore it in unloaded phases above several hundred fps, where it is mostly noise. |

## Limits

The window does not reproduce CS2-specific presentation behavior (Reflex,
independent flip transitions, overlays). Validate production capture-policy
changes against a CS2 demo with a fixed smoke-heavy round before release.
