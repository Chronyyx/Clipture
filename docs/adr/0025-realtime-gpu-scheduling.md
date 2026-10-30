# ADR 0025: Realtime GPU scheduling class for recording

Date: 2026-10-30

Status: implemented in 1.8.3; on by default, user setting "Prioritize recording on the GPU".

## Context

Clips froze for hundreds of milliseconds to several seconds while a game held
the GPU near 100 %, typically with uncapped FPS. It reproduced on Pascal
(GTX 1080, Marvel Rivals at ~100 FPS), Blackwell-class (RTX 4080 SUPER, CS2 at
350 FPS) and Ampere (RTX 3080, Overwatch uncapped at 97-98 % GPU), all with
hardware-accelerated GPU scheduling (HAGS) on. The game itself ran smoothly.

The diagnostics signature, together: NVENC output-event wait p95 50-70 ms and
more, `nvEncEncodePicture` itself blocking tens of milliseconds, all 16 NVENC
frames in flight, capture-slot exhaustion in the hundreds per sample, WGC
capture falling to 8-35 FPS. No encoder-queue backlog from the CPU side and no
disk backlog. The engine's GPU work is small; it was waiting behind the game's
work at the same scheduling priority.

`IDXGIDevice::SetGPUThreadPriority` (+1 since earlier releases) only orders
contexts within one process's priority band. The process scheduling class
(`D3DKMTSetProcessSchedulingPriorityClass`) orders processes against each
other, the foreground game included. Windows grants `high` and `realtime` to an
unelevated process.

Measured on the RTX 3080, Overwatch, 1440p60 NVENC, same settings:

| Class | Minutes | Encoder queue full | Slot drops | Clip frames fresh |
|---|---|---|---|---|
| normal (1.8.2) | 30 | 29 samples | 2,543 | freezes |
| realtime | 23.7 | 0 | 0 | 100 % |
| high | 15.1 | 380 samples | 30,834 | 46-50 % |

`high` was far worse than leaving the default with HAGS on. OBS selects `high`
when HAGS is on and `realtime` otherwise; Sunshine defaults to `realtime` and
offers `high` because NVIDIA drivers can freeze NVENC, or crash, with realtime,
HAGS and VRAM close to full.

## Decision

- The engine applies the `realtime` class when `gpuPriorityBoost` is on (the
  default) and `normal` when off, on every `configure`, so the setting applies
  live. `high` is never chosen. `CLIPTURE_GPU_SCHEDULING_CLASS` overrides it for
  engineering A/B runs. Per-device thread priority stays at +1.
- The host supervises the encoder from outside the engine
  (`engine/encoder_watchdog.rs`): it polls encoder counters each second and
  declares a hang when output stops while frames are pending, or when the
  engine stops answering, for 3 s at realtime or 15 s at normal (contention
  alone can pause output that long). Saves and configures pause the clock.
- The first hang terminates the engine; the existing supervisor restarts it and
  the boost stays off for the session. A second hang halts recording until
  Clipture restarts; Clipture tells the player both times with a native notice.
  The replay buffer of a hung engine is lost.
- Diagnostics record the requested and applied class, HAGS state, driver
  version, the process VRAM budget and usage, adapter-wide dedicated VRAM use
  (every process), and the session's recovery state.

## Consequences

- Recording keeps its frame rate when a game saturates the GPU. The game yields
  the GPU to Clipture's short jobs; its FPS cost has not yet been measured in
  live games (the stress window showed a few percent of frame time).
- An NVENC freeze under realtime costs a few seconds of recording and the
  buffer, not the session; a driver crash (TDR) is outside Clipture's control.
- The capture smoke test freezes the engine process to exercise recovery and
  halt; a real driver hang has not been reproduced.
