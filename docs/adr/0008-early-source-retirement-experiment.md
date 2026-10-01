# ADR 0008: Early source retirement before encoder conversion

Date: 2026-09-14. Status: opt-in experiment; not a production default change.
2026-09-17 update: first matched-settings smoke trial collected for the
candidate only; a control pass is still required before any promotion or
rejection decision. See the September 17 trial section below and
`docs/cs2-capture-theory-tests.md`.

## Evidence and decision

The timestamp-corrected WGC smoke run delivers 78-81 distinct source IDs/s at
99-100% GPU utilization. Capture slots run out 436/476 times in two ~14-second
windows, NVENC CPU p95 reaches 26-28 ms and encoder queue residence p95 reaches
226-233 ms. CPU cadence wakeups remain healthy. This establishes backpressure,
not which GPU engine, driver lock or scheduling boundary causes it.

ADR 0006 signals source completion after canonical color conversion. Test one
alternative: `CLIPTURE_EARLY_SOURCE_RETIRE=1` copies selected shared BGRA into
one private encoder-device scratch texture, signals source completion and
flushes, then performs the unchanged canonical NV12 conversion from scratch.
Control (`0`, default) retains direct shared-source conversion. Both retain the
same isolated encoder device, backend, cadence, quality and queue capacities.

`EncoderSourceSnapshot.*` owns validation, scratch reuse and the copy/retirement
ordering. It only runs for isolated direct-reader conversion, not native BGRA
or shared-device/bridge fallbacks. Repeated source IDs still use the canonical
cache. Allocation occurs before arming a source read; errors after arming fail
closed. Scratch is reset with the prepared-input resources on session changes.
Subsequent conversion and scratch overwrite are ordered on the same immediate
context. No new producer wait, CPU fence wait, polling thread or elevated
priority is introduced. Source CPU leases still retain their existing lifetime;
this experiment changes the GPU-read boundary, not job scheduling.

## Tradeoff and interpretation

One additional 2560x1440 BGRA texture is 14.06 MiB of pixel storage, plus driver
overhead, and one additional full-frame GPU copy runs per selected fresh image.
This is deliberately not assumed faster: added bandwidth may worsen saturated
GPU behavior. No replay RAM, capture pool, 64-job queue, 32-output-slot pool,
audio, save behavior, tonemapping, bitrate or rational 100 ns cadence changes.

If slot drops and freshness improve at matched GPU load, the shared-source
lifetime through conversion was contributing. If the candidate does not help,
do not promote it; prior encoder work / GPU scheduling / submission delays may
dominate before the new copy can even execute. Short GPU timestamp intervals
exclude queue time before their first marker. Query pending age includes time
between CPU polls and is not an independently measured GPU fence latency.
Canonical-conversion timestamp duration remains unvalidated on this driver.

## Verification and experiment

- WARP A/B test parks downstream work. Control protects the source; candidate
  retires it before the parked work. Overwrite the retired source and verify
  exact original output pixels. Test invalid ownership and scratch reuse.
- Existing reader tests park work before source copy and verify no early reuse,
  no producer dependency, and guards surviving reader/epoch destruction.
- `node scripts/performance/probe-wgc-cadence.cjs --early-retire` runs control /
  candidate / control at 120 FPS, traced candidate, DXGI, scaled NV12, BGRA and
  shared-device fallbacks, and 144 FPS. Require mode activation, decode, output
  throughput, clean shutdown, zero measured slot/queue/scheduler drops.
- After compatibility passes, compare matched WGC clear / smoke / clear trials
  with identical 120 FPS settings, trace setting and GPU sampling. Use distinct
  source rate, slot drops, NVENC tails, queue residence and game FPS, not MP4
  nominal FPS. Repeat control afterward to check scene/thermal drift.

## September 17 first user smoke trial (candidate only)

The validated candidate (SHA-256 `944CA1E5...0313`) was launched via the
verified staged package with tracing; the first export was discarded because
the profile was still at 60 FPS, and the corrected 120 FPS run is the only
trial analyzed. A passive read-only diagnostics monitor was left
running; no further sampling was started during gameplay.

- Inputs: `Clipture diagnostics 2026-09-17T11-01-00-081Z.json`,
  `Counter-Strike 2/Clipture 2026-09-17 07-00-04 AM.mp4` (120 s, 2560x1440,
  120 FPS, NVENC P3, 50 Mbps), trace
  `.cache/cs2-perf/retire-candidate-20260917-03.stderr.log`, GPU samples
  `retire-candidate-gpu-20260917-120fps-02.json` (1182 samples), correlation
  `retire-120fps-cluster-01.json`, frame `retire-120fps-at70.png` (dense smoke
  near clip second 70). The replay retained ~16 seconds of 60 FPS capture
  before the settings change; comparisons use only the 120 FPS history
  (10:58:18.717 UTC onward, 12,480 samples / 104 s, every bucket 120 samples,
  maximum gap 8.3334 ms).
- Cadence: output stayed rational at ~120 packets/s with zero encoder queue
  drops, scheduler deadline misses, backpressure or NVENC input/surface drops.
  Distinct encoded sources averaged 101.625 FPS over the 120 FPS section with
  15.31% repeats; all 255 reported drops in that history were
  capture-slot-exhaustion, clustered at 10:59:05.115-27.255 UTC (228 drops,
  22.14 engine seconds) plus two small clusters near 10:59:53-59.
- Smoke evidence: an extracted keyframe near clip second 70 shows dense smoke.
  The principal cluster's sampled GPU-wide utilization median was 94%
  (67-95%), NVENC encoder 43% median, graphics clocks 1920-1935 MHz: high
  contention, but the encoder is not the saturated engine.
- Candidate health: private-copy/consumer-wait/conversion GPU-health counters
  show no pending backlog or skipped queries. Private-copy weighted average
  ~0.05 ms (max 8.67 ms); canonical conversion timestamps remain ~0 and
  unvalidated. The notable signal: `source-consumer-wait` reached 45.09 ms
  with 3 samples over 10 ms in the full trace (2 samples over 5 ms, max
  9.72 ms within the principal cluster). This stage is the encoder context's
  `Wait(readyConsumer_)` for the capture context's ready signal, measured
  with the existing sparse sampler, and it does not include queue delay
  before the wait marker.
- Interpretation: the extra private copy is measurably cheap and the retire
  ordering is safe in practice, but slot reuse still waits for the full
  consumer sequence (ready wait -> copy/retire -> consumed signal). A 45 ms
  stall in that chain freezes slot recycling for multiple 8.33 ms cadence
  slots, which is consistent with the observed exhaustion. This does not
  establish whether early retirement helped or hurt relative to control; no
  matched control run exists yet. Pending: one matched control smoke trial
  with identical settings/tracing/sampling, then a control repeat for drift.

## API basis

[CopyResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource)
is an asynchronous compatible-resource GPU copy, not a CPU completion guarantee.
[Context Signal](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-signal)
updates the fence after preceding work completes. Keep the existing GPU fence
guard: returning from CopyResource alone must never authorize source reuse.
