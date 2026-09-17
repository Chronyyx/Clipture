# Remaining smoke freshness: theory checks and timing probes

Date: 2026-09-13. Follow-up to
[the capture investigation](cs2-capture-performance-investigation.md) and ADR 0005.

## What the existing smoke trial establishes

The 04:00:27 clip has about 55-56 distinct images/s in two smoke passages,
despite 120 output samples/s. Selected interior-smoke telemetry windows contain
1,528 fresh publications and 3,222 accumulated updates beyond the first over
25.994 seconds. No encoder queue drops or scheduler skips occur. Successful
DXGI and NVENC CPU call p95 values are now sub-millisecond. This is not the
original simultaneous 43/39 ms CPU-tail failure.

The existing export omits latency for unsuccessful DXGI acquisition attempts
and GPU execution/wait time. It cannot identify the remaining cause by itself.
No synchronized GPU-utilization or presentation-mode trace exists for that clip.

## Assessment of the six proposed theories

| Theory | Evidence and qualification | Discriminating observation |
| --- | --- | --- |
| Idle timer misses updates | Possible, not established. A 0.5-1 ms wait alone does not necessarily miss every second 5.5 ms presentation. Earlier yield polling cost roughly one CPU core without improving desktop output cadence. | Compare actual idle duration and full acquire-cycle time in the same smoke scene; then backoff/yield A/B with everything else fixed. A yield improvement alone is not proof of timer coarseness. |
| DXGI/DWM coalescing | Accumulation is established, but not its cause. `AccumulatedFrames > 1` means updates accumulated while the application processed the previous desktop update. This also happens when the application's GPU work cannot keep up. | Correlate accumulation with cycle, GPU copy, and fence timings. Use presentation tracing if those remain inconclusive. |
| Capture GPU copy delay | Plausible. Fast CPU submission does not establish fast GPU completion. The claim that `ReleaseFrame` must synchronously wait for the copy is not established by the API contract. | Sparse GPU timestamps around raw copy, CPU copy/release timings, and pending-query age. |
| Cross-device fence dependency | Plausible. The bridge is reused after the consumer's canonical conversion, not after NVENC bitstream completion. A slow consumer can still delay producer GPU work on that context. No CPU wait or shallow blocking lease pool has been demonstrated. | Time producer wait, bridge copy and consumer wait separately; measure capture lease acquisition and existing input preparation/queue counters. |
| WDDM priority starvation | Physical-GPU contention is plausible but utilization in heavy smoke load was previously unknown. Capture and isolated encoder already request relative priority +1. The assertion that Reflex specifically grants CS2 a higher WDDM priority is unproven here. | Aligned GPU-wide utilization/clocks plus stage timings; only then a separately scoped priority A/B measuring game performance too. No default priority increase. |
| Independent/composed flip transition | Possible with overlays/window composition changes. Smoke pixels inside the game's rendered image are not themselves a transparent desktop overlay. No presentation trace yet connects a mode transition with smoke. | PresentMon/ETW `PresentMode` correlated with a marked smoke interval. No Windows/game presentation settings changed speculatively. |

Microsoft's [frame-information contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_outdupl_frame_info)
defines accumulation; `RectsCoalesced` concerns dirty regions, not flip mode.
The [ReleaseFrame guidance](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-releaseframe)
also recommends minimizing release-to-next-acquire time, to avoid redundant
desktop updates. That is a separate release-policy experiment if measurements
point there, not a reason to assume a hard 60 Hz copy limit.

Microsoft documents the [risks of priority changes](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgidevice-setgputhreadpriority)
and [flip-mode eligibility/composition](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model).

## Opt-in implementation

`CLIPTURE_PIPELINE_TRACE=1` enables process-local stderr diagnostics. Absent or
other values leave probes off. No persisted setting or host-protocol change.

- `PipelineTimingTrace.hpp`: per-thread CPU aggregation, approximately one
  record/second/stage, including UTC, count, mean, maximum and counts above
  1/5/10 ms. This is not a p95 estimate.
- `GpuStageProbe.hpp`: bounded eight-slot timestamp/disjoint-query ring per
  stage, at most ten sampling attempts/second/stage. Delayed queries remain
  pending; full rings skip sampling rather than wait. Invalid/disjoint samples
  are excluded and counted. Readback uses `DONOTFLUSH`, without busy polling,
  CPU fence waits, or additional GPU flushes.
- CPU stages: successful acquisition, timeout acquisition, actual idle wait,
  acquire cycle, texture lease, copy submission and release.
- GPU stages: capture copy, selected HDR tone map, producer fence wait, bridge
  copy, consumer fence wait and canonical NV12 conversion.
- Health records expose pending query count/oldest age and completed/skipped
  counters. Absence of completed timings does not mean a stage is fast.
- GPU query ownership follows the capture, tonemapper and encoder/handoff
  lifetimes; GPU objects are not retained in thread-local destructors. The
  hardware harness also rejects watchdog termination/nonzero engine exit.
- `summarize-pipeline-trace.cjs`: reconstructs Tauri chunk-prefixed logs,
  validates records, reports rejects, filters observation UTC and produces
  weighted stage summaries plus original records. Refuses overwriting reports.

Capture/encoder algorithms, rational 120 FPS deadlines, 64-job queue, surface
counts, audio and saved-profile settings are unchanged by this diagnostic work.

### Measurement limitations

Timestamps measure elapsed GPU time between markers, potentially including
preemption or dependency waits, not solely shader/kernel execution. They exclude
time queued before the first marker. Readback occurs later: UTC is the CPU
observation window, not calibrated GPU issue time. Never subtract timestamps
from different devices. Frequency/disjoint validation follows Microsoft's
[query documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query).

Instrumentation itself adds query commands, sparse context locking and logging.
Disjoint scopes are serialized on each immediate context. Tracing must be
compared against tracing off; it is not a production performance optimization.
Sparse sampling can miss short stalls. GPU-wide `nvidia-smi` values do not
attribute utilization to CS2 or measure individual game-frame times.

The initial hardware run returned mostly zero-duration canonical-conversion
timestamp pairs. Treat that probe as unvalidated for measuring video-processor
execution on this driver, not evidence of free conversion. Microsoft's
[VideoProcessorBlt contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videocontext-videoprocessorblt)
explicitly qualifies query accounting at feature levels below 11; that caveat
alone does not explain this device. Fence, CPU submission and eventual ETW
measurements remain necessary if conversion/dependency delay is implicated.

## Reproduction and handoff

1. Verify one recorder. Keep 120 FPS, resolution, HDR, bitrate/preset and all game
   settings constant. Launch the tested development recorder with the trace
   environment variable inherited only by that launch.
2. Capture vendor samples during the actual trial:
   `node scripts/performance/sample-gpu.cjs 180 .cache/cs2-perf/smoke-timing-gpu.json`
   (choose a new filename for each run).
3. Turn slowly for about 20 seconds clear, then 20 seconds inside smoke, then
   clear again. Save and immediately export diagnostics, without restarting.
   The extra timing traces live in the development stderr log, not in that JSON.
4. Align saved-clip cadence, host history, trace UTC and GPU samples. Identify
   whether the long interval is idle/timeout, CPU submission, GPU copy or a fence
   dependency before choosing an implementation change.
5. If idle is implicated, repeat a matched trial with
   `CLIPTURE_CAPTURE_IDLE_BACKOFF=0`, then restore the default. If GPU timing is
   short but acquisition remains sparse, proceed to PresentMon/ETW rather than
   guessing DWM mode from scene pixels. No PresentMon/ETW trial has been run yet.

Tests: `ctest --test-dir build/engine -C Release --output-on-failure` and
`node scripts/performance/test-pipeline-trace.cjs`. Explicit hardware sanity:
`node scripts/performance/probe-pipeline-timing.cjs` (close the daily recorder
first). It uses isolated profiles, 120 FPS, tracing off/on/yield/off-repeat and
decodes every saved video; it does not edit the installed profile.

## September 13 validation and approved relaunch

Native suite: 10/10 passed, including trace enabled/disabled, GPU handoff,
HDR/cursor pixel equivalence, rational cadence, audio and replay. Trace parser
tests passed; `git diff --check` passed (existing CRLF notices only).
Build succeeded with the existing constant-condition warning and optional
post-build `pwsh.exe`-not-found message; actual executables were exercised.

Initial hardware artifact `.cache/cs2-perf/timing-probe-yYo5nW/report.json`
preserves a **rejected** instrumentation build (`421B9B0C...`): all files decoded
and output was ~120 FPS, but traced runs reached the shutdown watchdog. The old
harness did not count that as a failed trial. Do not present those runs as full
passes. Replacing thread-local GPU-resource owners with pipeline-owned probes
resolved the observed shutdown failure. The harness now explicitly records and
rejects watchdog/nonzero exits, and a fresh full comparison was run.

Validated candidate SHA-256:
`6295518B4505664CDB8C0A957F0D4CA7C63506444CB658D7D21A2083B2220BAB`.
Artifact: `.cache/cs2-perf/timing-probe-FoBme1/report.json`.

| Trial | Output FPS | Distinct FPS | CPU core equivalents | Clean EOF exit |
| --- | ---: | ---: | ---: | ---: |
| Trace off | 119.981 | 59.34 | 0.243 | 156 ms |
| Trace on, default backoff | 119.990 | 43.73 | 0.210 | 137 ms |
| Trace on, yield polling | 120.076 | 43.74 | 1.195 | 131 ms |
| Trace off repeat | 119.972 | 44.31 | 0.155 | 126 ms |

All four: zero reported drops, clean saved-video decode, normal exit code zero,
no watchdog. Traced runs yielded 294/298 parseable records with zero rejects.
Desktop content was uncontrolled: changing distinct FPS across these sequential
runs is not a causal instrumentation-overhead estimate or a matched smoke A/B.
It also prevents claiming a CPU saving from the traced build.

Default-backoff trace: mean actual idle 0.559 ms, maximum 2.160 ms; full acquire
cycle maximum 4.252 ms. Sampled GPU copy mean 0.0504 ms, maximum 0.0522 ms;
producer/consumer wait maxima 0.793/0.687 ms. No retained health sample showed
pending-query backlog or skipped queries. These are desktop observations, not
evidence that those stages remain fast during the marked smoke scene.

The approved development relaunch at approximately 18:47:58 UTC used controller
33232 and engine 33072, with no duplicate recorder. Normal profile hash stayed
`D66707E173008E91B5ACB547C267675BA249D82DA6B19B3A7D89D6EC85F50ADA`.
Test candidate staged only in `release/tauri-unpacked`; installed binaries and
saved clips untouched. Previous staged engine preserved at
`.cache/cs2-perf/before-timing-trace-096E0851.exe`.

Logs: `.cache/cs2-perf/timing-app-20260913.stderr.log`. All CPU/GPU trace stage
types verified after launch, with no parse rejects in the initial check.
A 180-second vendor sample was synchronized with the
actual trial at `.cache/cs2-perf/smoke-timing-gpu-20260913-01.json`.
The marked trial completed during this turn; results follow.

## Synchronized smoke trial (18:51): bridge dependency is the leading target

Inputs: `Clipture diagnostics 2026-09-13T18-51-27-243Z.json` and
`Counter-Strike 2/Clipture 2026-09-13 02-51-16 PM.mp4` (770,065,777 bytes).
The contact sheet shows menu/load time first, then two smoke passages with
clear gameplay between them. An extracted frame at visible 105 seconds shows
dense smoke and the game HUD reporting average 154 FPS; this is a sampled HUD
reading, not an independent PresentMon trace. Do not treat menu-time minima as
smoke FPS or compare this scene causally against the earlier day's trial.

Vendor sampling completed: 356 samples, 18:50:12.322-18:53:12.040 UTC. Both
smoke windows below are covered. Analysis uses only aligned window samples,
not the full-run median (which includes later playback/analysis activity).

| Measurement | First smoke | Clear between | Second smoke |
| --- | ---: | ---: | ---: |
| UTC selection | 18:50:23-42 | 18:50:43-54 | 18:50:57-18:51:12 |
| Complete engine-history seconds | 18.110 | 10.044 | 14.094 |
| Fresh publications/s | 55.99 | 115.59 | 49.17 |
| Distinct encoded sources/s | 53.34 | 94.78 | 47.96 |
| Output packets/s | 119.88 | 120.07 | 120.05 |
| DXGI supply estimate/s | 154.67 | 214.26 | 161.98 |
| GPU-wide utilization median | 99% | 96% | 99% |
| NVENC utilization median | 44% | 47% | 43% |
| Graphics clock median | 1905 MHz | 1905 MHz | 1905 MHz |
| Producer-wait sampled GPU mean | 5.160 ms | 0.531 ms | 7.217 ms |
| Producer-wait sampled GPU maximum | 53.220 ms | 17.745 ms | 42.567 ms |
| Producer-wait samples above 10 ms | 40 / 178 | 2 / 110 | 38 / 128 |
| Consumer-wait sampled GPU mean | 1.219 ms | 0.144 ms | 0.888 ms |
| Consumer-wait sampled GPU maximum | 53.455 ms | 7.797 ms | 51.910 ms |
| Capture-copy sampled GPU maximum | 0.0553 ms | 0.0539 ms | 0.0485 ms |
| Tone-map sampled GPU maximum | 0.0491 ms | 0.0573 ms | 0.0386 ms |
| Actual idle CPU mean | 0.581 ms | 0.584 ms | 0.576 ms |
| Full acquire-cycle CPU maximum | 2.315 ms | 6.303 ms | 1.950 ms |

Zero reported drops/encoder queue drops in all three selected windows. The
long GPU intervals are localized around handoff waits, not raw-copy or tone-map
execution. Idle duration does not worsen in smoke; the clear interval actually
has the larger maximum. This argues against idle-timer coarseness being the
dominant cause in this trial. The measured GPU load now supports physical-GPU
contention **for this trial**; it must not be projected onto September 12.

The leading architectural concern is the producer's
`Wait(doneProducer_, sequence_)` on **the same immediate context used by capture**.
Although the CPU call returns, work queued behind it, including later raw
capture copies, can be delayed until consumer progress. Fast timestamps around
the copy itself do not include that preceding queue delay. This is the concrete
path through which encoder-side GPU scheduling can reduce capture freshness
without recreating the old CPU-call tails or filling the encoder job queue.

This strengthens theory 4 and the shared-GPU scheduling part of theory 5. It is
not proof that every microsecond between wait markers was blocked on an
unsignaled fence: GPU preemption/scheduling also contributes. The conversion
probe remains unvalidated (near-zero timestamps), and no ETW presentation-mode
trace was collected. Theory 2's accumulation is an observed consequence; its
exact compositor contribution remains unresolved. Copy *execution* is not the
long sampled stage, but copy *queueing* remains implicated.

### Next architectural experiment, not silently enabled

Remove consumer-dependent waits from the capture context. Keep acquisition's
GPU work one-way (owned copy + ready signal); consume immutable leased inputs
and perform preparation on the encoder side. Fence-aware resource reclamation
must prevent overwriting surfaces still read by the consumer, without inserting
that consumer's wait ahead of new capture copies. Evaluate this as an isolated
engineering variant before changing defaults. Merely adding more bridge slots
or raising GPU priority does not establish a fix and can mask/shift contention.

Preserve rational 120 FPS cadence, audio, replay limits and job/surface counts;
measure game performance, source freshness, image integrity and resource
lifetime under matched smoke. Physical-GPU saturation can still limit achievable
freshness, so do not promise 120 unique images solely from the architectural change.
No further production capture-policy change was made during this investigation.

Artifacts: frozen trace `smoke-timing-app-01.log`; correlations
`smoke-timing-first-01.json`, `smoke-timing-clear-01.json`,
`smoke-timing-second-01.json`, all in `.cache/cs2-perf/`. The reusable
`correlate-pipeline-window.cjs` selects complete engine windows, excludes resets,
and preserves trace records; its interval/unit-conversion tests pass. All three
correlations have zero rejected trace records. Video overview and 105-second
image are `clip-145116-overview.png` and `clip-145116-at105.png`.

## September 13 follow-up: direct-source reader implemented

The preceding next-step proposal is now implemented in
[ADR 0006](adr/0006-direct-capture-texture-reader.md). The default opens leased
prepared capture textures directly on the encoder device. The capture context
signals readiness but no longer waits for encoder consumption or copies into a
reusable bridge. Per-slot GPU completion guards prevent premature reuse without
blocking capture. HDR/cursor preparation remains once per selected source on
the protected capture context; it has not been moved to a new shader device.

No cadence, audio, recording-quality or replay settings were changed. The old
bridge remains available with `CLIPTURE_DIRECT_TEXTURE_READ=0` and as a capability
fallback. Idle backoff remains enabled: the matched trace did not implicate it
as the dominant changing cost, and earlier yield tests increased CPU consumption.
GPU priority, game settings and capture buffer capacities were not raised.

Validation of engine SHA-256
`5007FA864F1DCFEBED79A8109564E018A99BD91FB9A9E58337A789A49C2EF562`:

- 12/12 native tests, including a forced consumer GPU stall, source-slot lifetime
  after reader destruction, exact image reuse checks, shared HDR/cursor pixels,
  rational cadence, audio and replay tests.
- Ten sequential NVIDIA HDR desktop trials in
  `.cache/cs2-perf/direct-reader-jMQm1o/report.json`: bridge/direct/traced direct
  120 FPS, scaled NV12/BGRA, keyed/shared-device fallbacks, direct 144/210/240.
  All clips fully decoded, all processes exited via EOF without a watchdog,
  and all measured recording, queue, scheduler and capture-slot drops were zero.
- Direct output rates were 120.013, 144.032, 209.950 and 239.922 packets/s.
  Shutdown took 118–161 ms across all variants. These are output cadence results,
  **not** claims of that many distinct desktop or game images.
- Host-contract, trace-parser, correlation tests and read-only artifact inventory
  passed. Native tests also ran during part of the desktop compatibility suite;
  these trials are not controlled CPU/GPU performance comparisons.

The approved development recorder was stopped and backed up as
`.cache/cs2-perf/before-direct-reader-5AC54D3C.exe` (actual pre-replacement hash
prefix, not the older timing-build hash). The tested engine was staged in
`release/tauri-unpacked` and relaunched with `--hidden`, direct reads enabled and
process-only timing diagnostics. The installed application was not modified.
Startup confirmed `handoff=direct-texture-read producerWait=false`, HDR capture
and one engine (PID 31896, controller 35440). Staged binary hash matches the
tested build. Settings SHA-256 remained
`D66707E173008E91B5ACB547C267675BA249D82DA6B19B3A7D89D6EC85F50ADA`.
The new trace is `.cache/cs2-perf/direct-reader-app-20260913-01.stderr.log`.

Remaining acceptance: repeat matched clear/smoke/clear at unchanged 120 FPS with
aligned GPU sampling. Check distinct-source rate, capture-slot drops and
`source-consumer-wait` timing; the old producer-wait stage should not execute on
the direct path. Removing the known dependency is established by code and the
forced-stall test; its improvement under heavy smoke load was evaluated in the subsequent direct-reader trial below.

## September 13 direct-reader matched smoke result

Inputs: `Clipture diagnostics 2026-09-13T23-52-54-113Z.json` in Downloads,
`Counter-Strike 2/Clipture 2026-09-13 07-52-29 PM.mp4`, and
`.cache/cs2-perf/direct-reader-smoke-gpu-20260913-01.json` (356 vendor samples,
180 seconds). The same development controller/engine remained running alone.
The 120-second clip's overview (`direct-reader-smoke-overview.png`, 5-second
spacing starting at clip second 45) confirms two dense-smoke passages with clear
gameplay between/after them. UTC alignment uses the save boundary around
23:52:29 and 120-second presentation length; it is approximate at subsecond scale.

| Measurement | Smoke 1 | Smoke 2 | Clear after smoke |
|---|---:|---:|---:|
| Selected UTC | 23:51:15–33 | 23:51:59–23:52:17 | 23:52:23–29 |
| Complete engine seconds | 16.079 | 16.067 | 4.017 |
| Fresh publications/s | 66.42 | 69.15 | 172.02 |
| Distinct encoded sources/s | 62.75 | 65.29 | 116.01 |
| Output packets/s | 120.03 | 120.06 | 119.99 |
| DXGI supply estimate/s | 175.26 | 182.98 | 233.26 |
| GPU-wide utilization median | 98% | 98% | 89% |
| NVENC utilization median | 45% | 45% | 48% |
| Graphics clock median | 1905 MHz | 1905 MHz | 1905 MHz |
| Consumer-wait sampled GPU mean | 0.308 ms | 0.122 ms | — |
| Consumer-wait sampled GPU maximum | 16.912 ms | 17.469 ms | — |
| Maximum window DXGI acquire p95 | 0.0916 ms | 0.1145 ms | — |
| Maximum window NVENC call p95 | 0.2878 ms | 0.2669 ms | — |
| Maximum window encoder queue residence p95 | 0.0228 ms | 0.0234 ms | — |

Both smoke selections have zero capture-slot exhaustion, queue overflow,
encoder queue drops, scheduler deadline misses and reported drops. Repetition
still fills missing fresh sources: 921 and 881 scheduler repeats respectively.
No producer-wait stage executes on the new path. Trace parsing rejected zero
records. Canonical-conversion GPU query values remain zero/unvalidated.

This is a partial improvement, not a 120-distinct-FPS fix: earlier smoke
selections had 53.34/47.96 distinct sources/s versus 62.75/65.29 now. Different
scene trajectories and GPU load (previous median 99%, now 98%) prevent treating
that difference as a precisely isolated speedup. The forced-stall test and live
trace establish removal of the explicit producer dependency; substantial capture
delivery loss remains without encoder or slot backlog. Next investigation must
separate DXGI/compositor delivery and pre-marker GPU scheduling delay, rather
than enlarge queues or infer a throughput ceiling from p95 call latency.

Correlations: `direct-reader-smoke-first-01.json`,
`direct-reader-smoke-second-01.json`, `direct-reader-clear-01.json` in
`.cache/cs2-perf/`. The GPU sampler finished normally; no test configuration or
recorder restart was performed during this trial.

## September 14: alternate-delivery implementation and interrupted validation

[ADR 0007](adr/0007-wgc-selected-frame-preparation.md) records the research and
WGC implementation. High GPU utilization does not make good recording inherently
impossible, but neither DXGI nor WGC promises a reserved slice or 120 unique
images under arbitrary load. We are comparing delivery APIs with equivalent
downstream preparation, not assuming a backend switch solves scheduling.

Engine SHA-256 `6BEAF75A5072270D8B503318D43C607F3DB0709BC1F660F1DEAD285D6BE1361D`
adds owned WGC HDR inputs, selected-frame tone mapping, direct shared reads,
safe source recycling/cropping and optional zero minimum-update interval. The
automatic monitor default remains DXGI. All 12 native tests passed, including
new WGC pixel/lifetime checks, and the host-contract checks passed.

Initial hardware report `.cache/cs2-perf/wgc-cadence-4pJwRj/report.json` contains
**five completed passes only**: DXGI 120, WGC 120, traced WGC, WGC 144 and scaled
WGC. The run ended incomplete at WGC BGRA; eager/shared-device trials were not
recorded. An earlier conversational statement that all eight passed was incorrect
and was corrected on resumption. The normal WGC path had passed before launch,
but that launch was not gated by a complete eight-trial report. The harness now
writes `complete:false`, the active trial and failure information as it runs,
and sets `complete:true` only after all eight pass. A new complete run is required.

The user-approved candidate launch at 02:16 UTC used WGC via a process-only
override, not a saved backend setting. No new smoke export was found on
resumption. The recorder had since been relaunched and the user had restored
the target from 144 to 120 FPS; these settings changes were preserved. The
current test work does not change game/driver settings, process elevation,
relative GPU priorities, recording quality or replay buffers.

### Resumption exposed a real fallback crash

The second run `wgc-cadence-eLgW7I` again stopped at BGRA. Do not attribute these
incomplete reports to the session limit: targeted reproduction
`wgc-bgra-check-OhkSYQ` exposed engine exit code 3221225477 (access violation).
The probe could miss an exit during its warmup delay and later await an RPC to
an already-dead child. With no live handles after its watchdog expired, Node
could exit zero without executing the remaining validation. It now rejects
requests after termination, rejects pending requests on watchdog/launch failure,
and writes stderr on cleanup; `test-probe-exit.cjs` reproduces the between-request
exit without running capture and verifies rejection plus preserved logs.

Code inspection found the encoder session-attempt reset clears `context_`.
NV12/scaled preparation recreates it, but native-size isolated BGRA bypasses
that initialization and calls `CopyResource` through the null context. Context
acquisition now precedes both the isolated and shared-device input paths. The
hardware suite adds native-size DXGI BGRA as well as WGC BGRA, for nine trials.
This is a correctness fix discovered during alternate-backend validation, not
an explanation for the prior NV12 smoke freshness limitation.

### Completed candidate validation and test launch

The corrected engine SHA-256 is
`B704BDB9DB71D7A0258F390090606C0CC2A2A5486CC209C22649830BA2540CB1`.
`wgc-cadence-aS9J5M/report.json` contains all nine passing trials and
`complete:true`: DXGI native NV12/BGRA, WGC native/traced/144 FPS/scaled/BGRA,
eager HDR and shared-device fallback. Every saved test clip decoded, measured
recording/queue/scheduler/capture-slot drops were zero, and EOF shutdown took
179–222 ms. WGC 120 output was 119.929 packets/s and WGC 144 was 144.022;
uncontrolled desktop freshness is not a smoke performance result.

All 12 native tests passed on this build; the new between-request exit regression,
host-contract suite, syntax checks and whitespace checks also passed. The previous
engine was retained as `before-wgc-bgra-fix-6BEAF75A.exe`. The complete-report
and matching-binary checks now gate deployment in the launch command.

At 09:22:58 UTC the approved candidate started as controller 36516 / engine
28496, with WGC monitor capture, direct texture reads and process-only timing.
Settings hash remained `D66707E173008E91B5ACB547C267675BA249D82DA6B19B3A7D89D6EC85F50ADA`
(120 FPS). No installed application files were modified. The new trace is
`wgc-candidate-app-20260914-02.stderr.log`; synchronized GPU sampling is
`wgc-smoke-gpu-20260914-01.json`, both under `.cache/cs2-perf/`.

### WGC smoke result and timestamp-provenance caveat

Input export: `Clipture diagnostics 2026-09-14T09-24-32-582Z.json`; clip:
`Counter-Strike 2/Clipture 2026-09-14 05-24-21 AM.mp4`. The ~82.104-second clip
ends around 09:24:21 UTC. A 5-second-spaced overview beginning at clip second 20
(`wgc-smoke-overview-01.png`) confirms clear gameplay followed by dense smoke
around clip second 60 onward. No clear-after segment was saved in this clip.
GPU sampling completed normally with 355 samples. Video extraction ran only
after the saved scene, so later decoder utilization must not be attributed to it.

| Metric | Clear 09:23:34–54 | Smoke 09:24:01–19 |
|---|---:|---:|
| Complete diagnostic seconds | 18.081 | 16.068 |
| Reported fresh publications/s | 229.47 | 114.58 |
| Reported distinct encoded sources/s | 118.58 | 87.81 |
| Output packets/s | 119.96 | 119.87 |
| GPU-wide utilization median | 90% | 99% |
| NVENC utilization median | 47% | 43% |
| Capture-slot exhaustion | 0 | 972 |
| Encoder queue drops | 0 | 0 |

The smoke interval has 684 non-monotonic source timestamps and 517 scheduler
repeats. Thus the reported ~88 distinct sources/s is **provisional**, not proof
of 88 unique game images: the shared timestamp selector repaired stale WGC
timestamps with current media time, then assigned fresh sequence IDs. A single
backward sample can also move that watermark ahead of later valid source times.
This can both inflate freshness attribution and spend copy/texture resources
on old images. Raw WGC duplicate/out-of-order frequency was not logged in this
build, so 684 repairs cannot be equated to 684 genuinely duplicated API frames.

GPU consumer-wait mean/max were only 0.010/0.0135 ms. WGC copy mean/max were
0.147/16.933 ms; tone-map mean/max 0.035/0.039 ms. Canonical conversion queries
remain unvalidated. Maximum per-window NVENC CPU p95 was 24.965 ms and queue
residence p95 83.932 ms. This is not the old producer-wait bottleneck. GPU-safe
slot reuse is limiting capture under backlog; no additional slots were allocated
to hide that pressure. Correlations are `wgc-smoke-heavy-01.json` and
`wgc-smoke-earlier-01.json` under `.cache/cs2-perf/`.

Before drawing a backend-performance conclusion, the next candidate adds a
WGC-only source timestamp gate **before copying or acquiring an owned slot**.
It rejects zero/duplicate/backward raw QPC times without advancing its watermark
or synthesizing timestamps. It resets at frame-pool epoch changes. WGC desktop
present counters now count accepted source timestamps rather than every callback
frame; acquired-update and rejection counters remain available. Rational output
cadence continues independently and may repeat the last accepted source.
`SourceTimestampGateTests` covers stale bursts, recovery and epoch reset; all
13 native tests pass. The existing DXGI timestamp behavior is not changed.

### Timestamp-corrected validation and post-reboot launch

`wgc-cadence-4vg1sE/report.json` completed all nine hardware trials successfully
on engine SHA-256
`773EC3C39203900CD898822B3185A9A23AF71F9DDE6124FF9F023216A46EECE0`.
WGC 120 output was 120.037 packets/s; WGC 144 was 144.042 packets/s.
These desktop checks establish compatibility, not smoke freshness.

After the user's reboot, no Clipture process was running. At 18:59:17 UTC on
September 14 the verified candidate was staged and launched as controller
15972 with the existing 120 FPS / save-in-place profile, WGC and process-only
pipeline tracing. The old staged engine is retained in
`.cache/cs2-perf/before-wgc-timestamp-B704BDB9.exe`; installed files were not
changed. Startup confirms WGC, isolated NVENC and direct texture reads.
Trace: `wgc-timestamp-app-20260914-01.stderr.log`; GPU sampling destination:
`wgc-timestamp-smoke-gpu-20260914-01.json`, under `.cache/cs2-perf/`.
The user was asked to save a clear / smoke / clear run and export diagnostics.
Results remain pending; do not treat the earlier provisional 88 FPS as verified.

### Timestamp-corrected user smoke results (18:59 UTC launch)

Collected automatically after the user run:

- Export: `Clipture diagnostics 2026-09-14T19-01-27-781Z.json` in Downloads.
- Video: `Counter-Strike 2/Clipture 2026-09-14 03-01-17 PM.mp4`.
- GPU sampling completed with 354 samples; settings hash remained
  `D66707E173008E91B5ACB547C267675BA249D82DA6B19B3A7D89D6EC85F50ADA`.
- The saved presentation spans 118.817 seconds, ending around 19:01:17.8 UTC.
  `wgc-timestamp-smoke-overview-01.png` samples every 10 seconds: clear gameplay
  precedes smoke, with dense smoke at approximately seconds 70/80 and 100/110.
  An intervening clear frame is visible around second 90. The correlated smoke
  windows below lie within those sections; video decoding began after the save.

| Metric | Clear 18:59:42-58 | Smoke A 19:00:32-48 | Smoke B 19:00:58-19:01:14 |
|---|---:|---:|---:|
| Complete diagnostic seconds | 12.064 | 14.059 | 14.058 |
| Fresh publications/s | 144.23 | 100.29 | 96.24 |
| Distinct encoded source IDs/s | 111.99 | 81.09 | 78.18 |
| Output packets/s | 120.03 | 119.92 | 120.43 |
| GPU-wide utilization median | 93% | 99% | 99% |
| Capture-slot exhaustion | 0 | 436 | 476 |
| Encoder queue drops | 0 | 0 | 0 |
| Scheduler deadline misses | 0 | 0 | 0 |

Correlations: `wgc-timestamp-clear-01.json`, `wgc-timestamp-window-a-01.json`
and `wgc-timestamp-window-b-01.json` under `.cache/cs2-perf/`.
Smoke raw timestamp rejections were 625/651: these are now excluded *before*
copy/publication, not repaired into new source IDs. Source timestamps establish
distinct delivered updates, not a pixel-level proof of unique game renders.
The result supports substantially better delivery than the original collapse,
but not stable 120 distinct FPS or a controlled percentage gain over DXGI:
the scenes/runs differ, and current clear WGC supply is approximately 144/s.

Remaining pressure is downstream of capture copying. Smoke A/B maximum
per-window NVENC CPU p95 is 28.024/26.247 ms; queue residence p95 is
226.367/233.249 ms, while scheduler wake p95 remains 0.411/0.360 ms. No encoder
queue overflow occurred. Capture-copy sampled GPU maxima are 0.0491/0.0481 ms,
tone-map maxima 0.0458/0.0397 ms and consumer-wait maxima 0.0126/0.0132 ms.
However consumer query results were still uncollected at ages up to 213/133 ms
in the health samples. Polling runs on the submit thread and is sparse; this
includes time before the next CPU poll, not just GPU execution or queueing.
Short measured GPU execution does **not** exclude long delays before that work
is scheduled or observed. Canonical-conversion GPU durations
remain zero/unvalidated and must not be interpreted as cost-free conversion.

Save completed in 100 ms, with zero save-window drops and no media payload
readback. This is not a persistence bottleneck. Next controlled target is
encoder-side GPU submission/completion and source-slot retirement under load,
not a larger replay buffer or a change to rational cadence. The tested WGC
candidate remains running; production backend defaults are unchanged.

### Next controlled experiment: retire shared sources before conversion

[ADR 0008](adr/0008-early-source-retirement-experiment.md) records the decision.
The production path signals source completion after canonical conversion.
The opt-in candidate makes one private encoder-side BGRA snapshot, signals
source completion after that copy, then converts the private image to NV12.
This isolates the shared-source lifetime across color conversion without
changing backend, NVENC format/preset, quality, scheduler or buffer capacities.
It may lose to the control if extra copy bandwidth or earlier queued work
dominates, so it remains disabled by default.

Native validation: all 14 CTest cases passed, including the new deliberately
blocked downstream A/B test. Candidate source retirement permits a safe source
overwrite before downstream completion while preserving every output pixel;
control remains protected. Existing tests cover blocked pre-copy reads and
session/epoch lifetime. Host contracts passed, including 35 browser operations,
8 engine commands and both frontend adapters. Read-only artifact inventory
completed without errors.

Hardware validation completed in
`.cache/cs2-perf/source-retirement-lrLWtf/report.json`: all nine trials passed,
all saved clips decoded, and all measured slot/queue/scheduler drops were zero.
Control / candidate / control output was 119.965 / 119.996 / 119.968 packets/s;
144 FPS candidate output was 144.037 packets/s. These are uncontrolled desktop
compatibility checks, not a smoke-performance comparison. Startup confirms the
early-retirement mode only for direct-reader NV12 trials; BGRA and shared-device
fallbacks correctly keep their existing paths. Traced private-copy GPU samples
averaged 0.0407 ms, maximum 0.0551 ms, with zero rejected trace records; this
does not predict scheduling delay under saturated smoke.

Candidate SHA-256:
`944CA1E58BEB58C968C1F9B246E837BF45829DE6BF32B88CC202F8D12CFE0313`.
Prepared separately at `release/tauri-source-retirement`, with validation
manifest. The previously tested WGC recorder was restored at 19:29:09 UTC
(controller 22648), and existing settings hash remained unchanged. Candidate
is **prepared, not activated**; installed binaries were not changed.

After exiting Clipture from its tray, launch from the repository:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/performance/start-source-retirement.ps1 -Mode candidate
```

Use `-Mode control` for the same binary without early retirement. The launcher
checks the binary hash and complete passing report, refuses another active
recorder, preserves saved settings, and confines engineering flags to its child.
Each launch creates a new stderr trace under `.cache/cs2-perf/`. Sample the same
clear / smoke / clear scene, save and export diagnostics for each mode; restart
only between completed trials, because unsaved replay clears on exit.

## September 17 candidate smoke trial and consumer-wait lead

The first user trial launched at 60 FPS by mistake and was excluded; the
corrected 120 FPS run (10:58:18.717 UTC onward in the 11:01 export) is the
analyzed trial. Full details live in ADR 0008; summary here:

- 120 output packets/s held; zero queue/scheduler/backpressure drops. Distinct
  sources 101.625/s (15.31% repeats); 255 slot-exhaustion drops, mostly one
  22-second smoke cluster (GPU median 94%, NVENC 43%). The candidate's private
  copy averaged ~0.05 ms with no pending backlog.
- New leading signal: `source-consumer-wait` (encoder context GPU wait for the
  capture ready fence) peaked at 45.09 ms, with 3 samples over 10 ms in the
  full trace. Slot reuse is gated behind the consumed signal that follows this
  wait in the retire chain, so stalls here directly freeze slot recycling.
  Sparse sampling caveat applies; this is not a p95 or a proven causal chain.
- Prior WGC smoke (September 14) had consumer-wait maxima of only 0.013 ms in
  the correlated windows, so the 45 ms observation is new under this trial's
  load; scene differences prevent direct comparison.

Updated next-step ranking (evidence-based, not yet experiments):

1. Matched control run for ADR 0008 (required for any promote/reject decision).
2. If slot exhaustion persists: instrument or restructure the retire chain so
   slot reuse does not depend on encoder-context progress - the wait itself
   runs on the encoder immediate context, so a stalled encoder delays
   signaling and recycling even when the copy is sub-millisecond. Any change
   stays opt-in behind a flag with a forced-stall native test, per the ADR
   0008 verification pattern.
3. Canonical-conversion GPU timestamps remain unvalidated (~0 on this driver);
   do not treat NV12 conversion as free in any accounting.

A passive read-only diagnostics monitor watches new exports in Downloads and
appends summaries to
`C:\Users\aidav\AppData\Local\Temp\kilo\clipture-diagnostics-watch-results.jsonl`
(user-requested; no sampling, decoding or recorder changes).
