# ADR 0005: Selected-frame preparation and isolated NVENC input

Date: 2026-09-13. Status: implemented; user smoke trial improved substantially,
but ~55-56 distinct FPS remains below the 120 FPS target. See investigation report.

## Context

The September 12 trial establishes simultaneous CPU-side DXGI and NVENC call
tails, queue overflow and collapsed distinct-image delivery, not measured GPU
saturation or a proven driver mutex. Healthy scheduler wakeups rule against
fixing this by changing the rational cadence or adding buffer headroom.

NVIDIA's [threading guidance and CUDA-use list](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/)
identify DXGI/DirectX encode interactions and RGB encoding's use of CUDA.
Avoid the shared logical device and its RGB input preprocessing where supported.
Neither change isolates the physical GPU from the game or guarantees driver calls
cannot block under load.

## Decision

1. Keep `AcquireNextFrame(0)` and the 0.2 ms no-spin idle backoff **outside** DXGI.
   Yield-only polling cost approximately one CPU core in the desktop A/B without
   improving output cadence. Do not add sleep to the rational encoder scheduler.
2. DXGI copies the acquired image into a leased texture and releases the acquired
   surface promptly. HDR tone mapping and cursor compositing run **once per
   selected source image** on the encoder submit worker, never on the cadence
   thread. Coalesced images do no shader preparation. Cursor snapshots retain
   their capture-time shape/position, and queued work owns old-epoch resources.
3. Give NVENC its own D3D11 device on the same adapter. A reusable shared texture
   transports prepared BGRA into that device, with two shared GPU fences:

   `capture copy -> ready signal -> encoder GPU wait -> conversion -> consumed signal`

   Before reuse, the producer GPU waits for the consumed signal. The signal is
   after reading/converting the bridge, **not after NVENC output completion**.
   Context flushes submit the signal commands. There is no CPU fence polling,
   blocking event wait, or keyed-mutex acquisition on this default handoff.
4. Prefer NV12 video-processor output, with the existing full-range Rec.709 RGB
   input / studio-range Rec.709 YUV output configuration. Cache a canonical
   converted image and copy into the existing leased per-output-slot surfaces.
   Repeated cadence ticks reuse preparation; never map the same in-flight
   NVENC input surface twice. H.264 quality/preset/bitrate remain unchanged.
5. Retain capability fallbacks: shared-device NV12 when shared fences are not
   available, and BGRA (including video-processor resizing) when NV12 is not
   supported. Legacy keyed-mutex isolation is an explicit engineering A/B only.

## Ownership and invariants

- `DeferredFramePreparation.hpp`: once-only result/error and captured-input lifetime.
- `DesktopFramePreparation.*`: DXGI HDR/cursor preparation; protects a complete
  shader-state sequence with the existing multithread-protected context.
- `GpuFrameHandoff.*`: two-device GPU fence ordering; no cadence/encode policy.
- `CapturePipelinePolicy.hpp`: process-only engineering overrides, not settings.
- `EncoderWorker`: chooses/consumes prepared images, canonical conversion and
  NVENC submission. Existing scheduler, 64-job queue and 32 output slots stay intact.
- No renderer/host protocol changes; no audio, replay, save-window or storage changes.
- Do not replace rational 100 ns deadlines with truncated 8 ms intervals at 120 FPS.

## Costs and limits

Deferred HDR needs independent leased raw HDR inputs; the old single scratch
texture is unsafe once capture and preparation run concurrently. This changes
GPU surface memory, not replay RAM limits: at 2560x1440 each additional RGBA16F
input is about 28.1 MiB. NV12 output surfaces are smaller than BGRA (about 5.3
versus 14.1 MiB each), while isolation adds a shared BGRA bridge and canonical
surface. Actual allocation depends on live leases and slots; do not claim zero
RAM/VRAM cost or a fixed net reduction. No queue capacities were increased.

Isolation trades extra copy/flush work for removing the DXGI device from NVENC.
The desktop tests establish correctness and throughput, not smoke causality.
They cannot promise 120 distinct images when DXGI delivers fewer or the physical
GPU is saturated. CPU timing measurements are not GPU execution timestamps.

## Verification

Native tests check rational cadence at every supported rate, once-only execution,
failure caching and discarded work; WARP pixel tests check HDR/cursor equivalence,
old-epoch lifetime and 64 queued cross-device images without premature reuse.
Hardware probes compare individual policy changes and decode saved media.
See `docs/cs2-capture-performance-investigation.md` for exact results and the
remaining matched CS2 smoke trial.
