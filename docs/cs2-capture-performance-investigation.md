# CS2 smoke / round-transition capture investigation

Date: 2026-09-11. Source baseline: `d6e494e` (1.5.4).

## Status

The remaining-freshness investigation now has a focused
[six-theory assessment and opt-in timing protocol](cs2-capture-theory-tests.md).
This preserves the historical results below and separates measurement work
from production pipeline decisions.

**Latest direct-reader trial (23:52):** producer wait removed; smoke freshness reaches ~63–65 distinct FPS (up from ~48–53 in the 18:51 baseline) while clear gameplay delivers ~116 distinct FPS. GPU utilization median is 98%. Consumer-wait GPU intervals average 0.12–0.31 ms (down from producer-wait 5.16–7.22 ms); encoder queue drops remain zero. Repetition still fills unsupplied frames (881–921 scheduler repeats). Next focus is separating DXGI/compositor delivery and pre-marker GPU scheduling delay. See the linked report for alignment, limits, tests and the remaining causal components.

### September 13 08:01 export: major stall removed, smoke freshness still ~56 FPS

Read-only analysis of `Clipture diagnostics 2026-09-13T08-01-45-874Z.json` and
`Counter-Strike 2/Clipture 2026-09-13 04-00-27 AM.mp4` (701,806,249 bytes).
Extracted an overview and individual images at visible 80 and 103 seconds;
both individual images show dense smoke. Sampled game HUD averages are 173 and
191 FPS respectively (not an independent game-present trace). The overview
contains browser/menu time before gameplay, so the whole-export or whole-clip
minimum is not a smoke-only metric. Original media was not modified.

The matching startup log confirms isolated NVENC, deferred preparation and
GPU-fence handoff. The clip stays configured at 120 FPS throughout. Mux preroll
is 1.3998322 seconds; subtract it from raw cadence bucket boundaries:

| Visible interval | Raw buckets | Mean distinct images/s | Per-second range | Output samples/s |
| --- | --- | ---: | ---: | ---: |
| 1:14.60-1:26.60, first smoke passage | 76-87 | 55.42 | 48-73 | 120 |
| 1:37.60-1:50.60, second smoke passage | 99-111 | 56.38 | 51-65 | 120 |

Previous severe-smoke passage delivered 5.33 distinct images/s and 100.5 output
samples/s, with 117 queue drops. This is a large improvement between trials,
not a controlled same-scene estimate of the contribution of each individual change.
Recovery outside smoke reaches 114-120 distinct images/s near the end of this clip.

Across this export's 155 approximately two-second windows (309.964 s): zero
reported engine drops, encoder queue drops, NVENC input/surface drops, capture
slot/overflow drops, scheduler skips, device access losses or epoch transitions.
Maximum sampled **successful-acquisition p95** is 0.1175 ms, and maximum sampled
NVENC-call p95 is 0.3469 ms. These are maxima of rolling p95 samples, not maxima
of every individual call. Startup has 114.46 ms queue-residence p95; excluding
that first initialization window, the maximum sampled queue p95 is 0.0307 ms.

Aligned interior-smoke windows ending 07:59:44-07:59:54 and 08:00:06-08:00:18 UTC:

- Successful DXGI acquisition p95: 0.0569-0.0793 ms.
- NVENC-call p95: 0.2152-0.3071 ms.
- Encode-job queue residence p95: 0.0184-0.0219 ms.
- CPU input-preparation p95: 0.5275-0.6656 ms.
- Scheduler-wake p95: 0.3252-0.4999 ms; no scheduler skips.
- Fresh publications: 49.57-70.21/s, versus DXGI present-plus-accumulation supply
  estimate 175.59-190.24/s. That supply estimate is not a GPU utilization measure.

The encoder is sustaining output but repeats the images it receives. The main
remaining loss of visual freshness is upstream of encoder submission, in delivery
of fresh capture images. Unlike the previous run, no long CPU-side DXGI/NVENC
tail-and-queue-overflow pattern appears in the retained telemetry. The counters
alone do not identify whether availability is limited by GPU copy/composition,
the cross-device handoff's GPU dependencies, or another presentation/driver path.
This trial did not have synchronized vendor GPU sampling.

Measurement caveat: current `acquireWaitLatency` records only successful
`AcquireNextFrame` calls, not `DXGI_ERROR_WAIT_TIMEOUT` returns. Fast successful
calls therefore do not prove every acquisition attempt was fast. Next profiling
should separate timings for success/timeouts and the full acquisition cycle,
then measure GPU execution/fence delay for capture copy, tone map, bridge and
conversion with nonblocking timestamp-query readback. Keep game settings, replay
limits and rational cadence unchanged. Do not infer physical GPU saturation or
enlarge queues from these counters.

Save took 303 ms with zero new drops; output cadence has no missing slots or
long gaps in the analyzed video sample sequence. Historical `saveTiming.tail`
inside the export was not used in place of the matching current-run stderr log.
Analysis outputs: `.cache/cs2-perf/clip-040027-overview.png`,
`clip-040027-at80.png`, `clip-040027-at103.png`. No recorder restart or capture
configuration change was made during this follow-up.

### September 13 23:52 trial: direct reader removes producer wait, smoke freshness reaches ~63–65 FPS

Inputs: `Clipture diagnostics 2026-09-13T23-52-54-113Z.json`,
`Counter-Strike 2/Clipture 2026-09-13 07-52-29 PM.mp4`, and
`.cache/cs2-perf/direct-reader-smoke-gpu-20260913-01.json` (356 vendor samples,
180 seconds). The development controller/engine ran in isolation.
The 120-second clip overview (`direct-reader-smoke-overview.png`, 5-second
spacing starting at clip second 45) confirms two dense-smoke passages with clear
gameplay between and after them. UTC alignment uses the save boundary around
23:52:29 and 120-second presentation length (approximate at subsecond scale).

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

Both smoke selections show zero capture-slot exhaustion, queue overflow,
encoder queue drops, scheduler deadline misses and reported drops. Repetition
fills unsupplied fresh sources: 921 and 881 scheduler repeats respectively.
No producer-wait stage executes on the new direct-reader path. Trace parsing rejected zero
records. Canonical-conversion GPU query values remain zero/unvalidated.

This demonstrates partial improvement rather than a complete 120-distinct-FPS resolution: earlier smoke
selections delivered 53.34/47.96 distinct sources/s versus 62.75/65.29 now. Different
scene trajectories and GPU load (previous median 99%, now 98%) prevent treating
that difference as a precisely isolated speedup. The forced-stall test and live
trace establish removal of the explicit producer dependency; capture
delivery loss remains without encoder or slot backlog. Next investigation must
separate DXGI/compositor delivery and pre-marker GPU scheduling delay, rather
than enlarge queues or infer a throughput ceiling from p95 call latency.

Correlations: `direct-reader-smoke-first-01.json`,
`direct-reader-smoke-second-01.json`, `direct-reader-clear-01.json` in
`.cache/cs2-perf/`. The GPU sampler completed normally; no test configuration or
recorder restart was performed during this trial.

### September 13: selected preparation + isolated GPU-fenced NVENC implemented

**Implementation and desktop validation complete; the smoke trials above
show substantial improvement but not full resolution.** This addresses the shared logical D3D11-device dependency
without changing the rational scheduler, capture quality, audio, save policy,
64-job queue or 32 NVENC output slots. See [ADR 0005](adr/0005-selected-frame-preparation-and-isolated-nvenc.md).

The default path now copies/relinquishes each DXGI surface promptly, performs
HDR/cursor work only for selected images (once across repeated ticks), transfers
to an independent encoder device through shared GPU fences, and converts to
NV12 before NVENC. Capture-time pointer snapshots and raw-HDR leases prevent
deferred work from reading a newer cursor/image. No keyed-mutex CPU wait is used
on the supported default path. Physical-GPU scheduling and driver-wide waits can
still occur; this is not evidence that the original event was GPU saturation.

#### Measurements

Explicit, sequential primary-display probes, HDR 2560x1440, H.264 P3/50 Mbps,
isolated workspace profile, no audio. The development controller/UI were
closed and its engine exited before testing. No game interaction
or controlled motion workload was generated. **Output FPS is not distinct FPS.**

Initial baseline/candidate report: `.cache/cs2-perf/cadence-probe-m9vvY0/report.json`.
Both decoded; output 119.712 / 119.965 FPS, no reported drops.

One-variable matrix: `.cache/cs2-perf/pipeline-nta05A/report.json`;
GPU sampling: `.cache/cs2-perf/gpu-pipeline-20260913.json` (UTC receivedAt aligns
with each trial's measuredFrom/measuredUntil; sampling began after the run started).

| 120 FPS policy (unless noted) | Output FPS | CPU core equivalents | Reported drops |
| --- | ---: | ---: | ---: |
| Existing preparation + BGRA, shared device | 119.980 | 0.269 | 0 |
| Deferred preparation only | 120.086 | 0.146 | 0 |
| NV12 only | 119.997 | 0.132 | 0 |
| Deferred + NV12, shared device | 120.064 | 0.233 | 0 |
| Deferred + NV12, legacy keyed-mutex isolation | 120.037 | 0.227 | 0 |
| Shared-device deferred/NV12, yield-only idle | 120.009 | 1.069 | 0 |
| Shared-device deferred/NV12 repeat | 119.974 | 0.241 | 0 |
| Shared-device deferred/NV12 at 240 FPS | 239.945 | 0.338 | 0 |

All eight saved videos decoded. Desktop publications differ between sequential
trials, so CPU differences do not establish matched-load speedups. This matrix
rejects yield-only polling as the default. The keyed transfer has approximately
0.586 ms final-window input-preparation p95 versus 0.104 ms shared-device in this
run; it is not an adequate reason to add synchronous waits under smoke load.

Implemented a two-fence GPU handoff next. Microsoft documents the
[shared fence primitive](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-createfence).
The producer waits on the GPU before reusing the bridge; the consumer waits on
the GPU before reading it. Conversion completion releases the bridge independently
of NVENC's asynchronous bitstream drain. Context flushes and extra GPU copies
still have a cost; removing explicit CPU waits does not make driver calls free.

Fenced-isolation report: `.cache/cs2-perf/cadence-probe-iMypMt/report.json`;
aligned GPU samples `.cache/cs2-perf/gpu-fenced-20260913.json`.

| Candidate target | Output FPS | Distinct source images/s | Reported drops | Video decode |
| --- | ---: | ---: | ---: | --- |
| 120 | 120.024 | 85.28 | 0 | Passed |
| 144 | 144.043 | 96.99 | 0 | Passed |
| 210 | 210.022 | 112.09 | 0 | Passed |
| 240 | 240.077 | 112.48 | 0 | Passed |

These distinct-image rates reflect an uncontrolled desktop, not a claim of
120-240 unique game frames. GPU-wide sampling spanned idle/setup/save/decode too;
its full-run maximum encoder utilization reached 100%, so that maximum must not
be assigned to the 120 FPS window. There was no vendor sampling in the original
September 12 smoke event and no GPU timestamp-query instrumentation in this patch.

Final default/fallback checks: `.cache/cs2-perf/pipeline-compat-vlyAcj/report.json`.
The final default 30-second measurement delivered 120.003 FPS with zero reported
drops and a clean decode. NV12 resizing, BGRA resizing, explicit legacy keyed
isolation, and shared-device fallback delivered 120.034 / 120.054 / 119.949 /
119.970 FPS respectively, all with zero reported drops and clean video decoding.
Both resized files were independently confirmed as 1280x720 H.264 yuv420p.

Eight native CTests pass, including real WARP shader/readback tests for HDR and
cursor equivalence and 64 queued two-device fence transfers. Existing cadence,
audio/mixer and MP4/replay tests pass. Host-contract tests and cadence-summary
regressions pass. Read-only artifact inventory completed without errors.
No renderer/host wire contract was changed.

Staged and relaunched the tested sidecar at
2026-09-13 07:56 UTC. Engine SHA256:
`096E0851CC37148B0625E847491526EF1025BD710622D1F47A7009E4893DE446`.
Both `build/engine/Release` and `release/tauri-unpacked` match. Running controller
PID 34820 and engine PID 37880 are in `release/tauri-unpacked`; only one engine
was present. Startup log `.cache/cs2-perf/gpu-fenced-app.stderr.log` confirms
deferred=1, idleBackoff=1, isolated=1, preferNV12=1, handoff=gpu-fence and normal
audio-source startup. Existing settings remain at 120 FPS, original saveFolder;
settings SHA256 unchanged across launch:
`D66707E173008E91B5ACB547C267675BA249D82DA6B19B3A7D89D6EC85F50ADA`.
Installed Program Files binaries and saved clips were not modified. The unrelated
pre-existing unpacked-build updater-endpoints warning remains in the startup log.

Engineering switches are process-only exact `0`/`1` overrides:
`CLIPTURE_DEFER_PREPARATION`, `CLIPTURE_NV12_INPUT`, `CLIPTURE_ISOLATE_NVENC`,
`CLIPTURE_GPU_HANDOFF`, `CLIPTURE_CAPTURE_IDLE_BACKOFF`; all default to `1`.
Unset them for the normal candidate. `GPU_HANDOFF=0` is the legacy keyed A/B,
not a recommended fix. Startup logs report selected preparation/device/handoff.

Remaining acceptance: same resolution/HDR/P3/bitrate, constant 120 FPS, one engine,
slow camera rotation inside smoke, save and export diagnostics immediately. Align
`sample-gpu.cjs` UTC samples with the one-second engine windows. Compare distinct
images, acquire/submit tails, queue residence/drops, and audio/video appearance.
If collapse persists, capture GPU execution timestamps/ETW before attributing it
to physical saturation versus driver-wide contention. Do not increase queues or
claim success merely because output cadence contains repeated frames.

### September 12 user CS2 smoke clip: severe stall confirmed

Read-only evidence: `Clipture diagnostics 2026-09-13T02-44-03-919Z.json`
from Downloads, and latest CS2 clip `Clipture 2026-09-12 10-43-25 PM.mp4`
(867,386,728 bytes). The current development startup log records its save at
02:43:25 UTC. The export contains timeline samples through 02:44:15 despite its
earlier exportedAt value; use sample timestamps. Its embedded saveTiming tail is
historical August data and was not used to diagnose this save.

Decoded a 24-frame overview plus frames at visible 97 and 100 seconds. Both
individual frames are inside dense smoke, with the game overlay reporting
approximately 188 and 168 average FPS respectively. These are sampled game HUD
values, not an independent PresentMon measurement. The original media was not
modified. Extraction completed without reported video decoding errors.

The clip contains a settings transition, **not a uniform 120 FPS recording**:
the controller timeline reports 60 FPS from startup until 02:42:46.195, then
120 FPS. The cadence buckets switch at raw second 82. The mux log records
1.8248871 seconds of decoder preroll, so subtract that from bucket positions
to align them with visible playback. The beginning being 60 FPS explains why
the whole-clip `missingFrameSlots=5059` figure is misleading: the analyzer
compares the entire mixed-rate clip against the final 120 FPS target. Do not
interpret that number as 5,059 actual engine drops.

The smoke failure nevertheless occurs **after** the switch to 120 FPS:

| Visible clip interval | Output samples/s | Distinct source frames/s | Largest output PTS gap |
| --- | ---: | ---: | ---: |
| 1:23.18-1:27.18, before severe slowdown | 120 | 109.50 | 8.33 ms |
| 1:28.18-1:34.18, approaching smoke | 120 | 72.17 | 8.33 ms |
| **1:37.18-1:43.18, dense smoke** | **100.50** | **5.33** | **58.33 ms** |
| 1:45.18-1:59.18, after recovery | 120 | 110.79 | 8.33 ms |

Raw buckets 101/102/103 have 79/87/97 output samples, but only five distinct
source frames each. A mostly repeated recording is therefore a real problem
even when output sample FPS remains much higher than five.

Aligned live telemetry at 02:43:03-02:43:09 shows simultaneous graphics/capture
and encoder submission stalls. At the worst sampled window (02:43:07.192):

- Fresh publications approximately 4.97/s, while the DXGI present-plus-
  accumulated-frame estimate remains about 169/s. This estimate is not a game
  frametime trace, but supports a capture delivery problem over a 5 FPS game.
- Successful DXGI acquire-call p95 **43.29 ms**; NVENC-call p95 **39.03 ms**.
- Actual encode-job queue residence p95 **532.63 ms**, queue depth 64;
  scheduler wake p95 only **0.511 ms**, zero scheduler skips in the stall.
- **117 encoder queue drops** across the three 02:43:05/07/09 windows
  (2 + 71 + 44). The six scheduler skips elsewhere in the export occurred
  around 02:44:05-07, after this clip was saved, and do not explain its smoke.
- No capture epoch change/access loss, no texture-slot or NVENC-surface drops,
  and replay archive backlog zero in these windows. The subsequent save took
  289 ms overall and reported no additional drops during its operation.

Conclusion: desktop probes did not reproduce this **confirmed game-load capture/
submission stall**. The earlier suspicion of shared graphics-device/driver
contention remains plausible, but CPU call timing alone does not establish the
exact lock or GPU workload. This sample also rules out blaming the earlier
two-recorder test setup as the sole explanation. It does not establish that the
latest patch caused a regression versus the previous build: the supplied older
clip is not a matched smoke comparison.

Next causal experiment should hold the cadence fix, resolution/HDR, encoder
preset, quality and buffer capacities constant and A/B only the new capture
idle-wait policy in the same smoke scene. Correlate GPU clocks/utilization and
actual GPU stage timing, then consider capture/NVENC synchronization isolation
if both polling variants stall. Do not increase buffers, lower recording
quality, or apply an unmeasured GPU clock/power workaround. No production code,
settings or running recorder were changed for this analysis.

### September 12 evening: single-recorder comparison completed

After closing the installed recorder and switching to the test
build, verified no Clipture process remained. Ran the isolated matrix with only
one engine at a time, then launched the staged app normally with the existing
profile. UTC measurement times were September 13 02:32-02:36 (September 12 EDT).

Artifacts: `.cache/cs2-perf/cadence-probe-mM39JB/report.json` and
`.cache/cs2-perf/gpu-single-recorder-20260912.json`. Each rate used the same
30-second requested window, approximately 31 seconds with diagnostics overhead,
2560x1440 HDR, P3, 50 Mbps and no audio. Desktop activity varied; this was not a
controlled-motion or CS2 trial. The probe now records exact measurement-start
UTC time and timestamps each diagnostic sample after its reply, improving
alignment with GPU samples.

| Engine / target | Output FPS | Distinct source FPS | Queue / scheduler / total drops | CPU cores |
| --- | ---: | ---: | ---: | ---: |
| Original / 120 | 119.79 | 2.20 | 0 / 0 / 0 | 1.065 |
| Test / 120 | 120.01 | 0.35 | 0 / 0 / 0 | 0.132 |
| Test / 144 | 143.99 | 32.03 | 0 / 0 / 0 | 0.247 |
| Test / 210 | 210.01 | 190.78 | 0 / 0 / 0 | 0.277 |
| Test / 240 | 240.00 | 182.90 | 0 / 0 / 0 | 0.203 |

All five saves decoded successfully and every probe engine exited. None of the
120 one-second candidate windows (30 per rate) fell below 98% of its target.
The baseline had one lower counter window despite its near-target overall rate;
these are completion-counter windows, not exact visual frame times. Baseline
queue-residence telemetry still has the previously documented held-frame-age
bug and must not be compared to corrected candidate residence values.

At test 210/240, final-window NVENC-call p95 was 0.334/0.335 ms and job-queue
residence p95 was 0.018/0.018 ms. Wake p95 stayed below 0.34 ms. The earlier
approximately 187 FPS stall did **not** reproduce. Removing the second recorder
removes an important confound but does not prove it caused the earlier failure;
the previous two-recorder repeat also passed and scene/clock conditions varied.

GPU sampling contained 433 observations. During matched 210/240 measurement
windows, graphics clocks averaged 1935 MHz and GPU-wide encoder utilization
averaged 83/95%. The 240 GPU sample covers 49 samples (about 25 seconds), not
the entire measurement window; the GPU sampler ended before that trial did.
The high encoder utilization argues against promising spare capacity at 240
under game load, even though this desktop probe met the output target. No GPU
clock, power, quality, buffer, audio or save-policy settings were changed.

Launch verification: controller 7120, UI worker 15340 and engine 29388 all under
`release/tauri-unpacked`, with no Program Files Clipture process. App and engine
hashes match the previously verified test revision. Settings SHA256 before and
after launch remains
`FFB6F1F5A33D431F0BD24B8128D0B7CACFA503D729873BE2830BB010348CF437`.
Startup log `.cache/cs2-perf/single-recorder-app.stderr.log` confirms capture and
configured audio-loopback startup. The unrelated local-build updater endpoint
warning remains. Playback retry tests, probe-summary tests and source checks
passed. No new production engine change was made for this comparison.

Next: user CS2 gameplay with this now-running test build and a fresh diagnostic
export. Actual GPU-stage timing / avoiding unused HDR work remain investigation
leads, not enabled changes. Do not characterize duplicated output on static
content as unique 240 FPS motion, or desktop success as game-load verification.

### September 12: longer probes and an intermittent high-FPS stall

The reported player screenshot came from the released build, as later
confirmed. Read-only process inspection found controller 28072, UI worker 18304,
and engine 28232 under `C:\Program Files\Clipture`. The released engine hash is
`4822269A0006F4E8C5AE39327E128A365425676C33B952E5FCA337EC9466488B`;
the staged test engine remains `84C02F11757687EE3BB4F2FF3875F62D9FA09F2369A6F88BBA83C2CE2359065C`.
No player implementation regression was established; its bounded-retry tests
passed again. Neither the installed app nor the test build was replaced or
restarted during this follow-up. No settings or capture buffers were changed.

Extended the isolated hardware probe with configurable rates/duration, one-second
diagnostic snapshots, scheduler/total-drop checks, distinct-frame rates and UTC
trial times. Added an offline window summary and regression tests for short
stalls hidden by average FPS, repeated frames, missing data and counter resets.
Summary windows are counter snapshots, not frame-by-frame display measurements;
brief output completion bunching and IPC timing affect their boundaries.

First matrix: 30-second requested measurement per rate (about 31 seconds including
diagnostics/CPU sampling), artifact `.cache/cs2-perf/cadence-probe-pnA3yL/report.json`.
Both this matrix and the repeat used 2560x1440 HDR, P3, 50 Mbps, no audio, isolated
workspace output, while the user's released recorder remained running.

| Engine / target | Output FPS | Distinct source FPS | Queue drops | CPU cores |
| --- | ---: | ---: | ---: | ---: |
| Original baseline / 120 | 119.99 | 72.17 | 0 | 1.184 |
| Test / 120 | 120.02 | 119.79 | 0 | 0.346 |
| Test / 144 | 144.00 | 143.20 | 0 | 0.322 |
| Test / 210 | 187.06 | 10.91 | 698 | 0.256 |
| Test / 240 | 187.92 | 11.00 | 1615 | 0.216 |

The high-rate failures are real counter-observed submission stalls, not just low
unique-frame counts on a static desktop. At 210, scheduler skips were zero and
wake p95 was 0.321 ms, while NVENC-call p95 reached 8.720 ms, successful DXGI
acquire calls approximately 7-8 ms, and correctly measured job-queue residence
p95 304.35 ms. Queue drops occurred in every one-second window. Input mapping
was much shorter (about 0.13 ms p95). This narrows the lead to graphics/encoder
submission or shared-device contention, not timer wakeup or replay disk backlog.
It does not prove which driver/GPU operation caused the CPU calls to block.

Repeated with the **same test executable for every trial**, including the row
labeled `baseline-120` by the harness. Artifact
`.cache/cs2-perf/cadence-probe-HgSAPR/report.json`; requested windows 15 seconds,
about 15.9 seconds with measurement overhead:

| Target | Output FPS | Distinct source FPS | Queue / scheduler drops |
| --- | ---: | ---: | ---: |
| 120 (first control) | 119.99 | 94.39 | 0 / 0 |
| 120 (repeat) | 119.98 | 56.28 | 0 / 0 |
| 144 | 144.00 | 127.85 | 0 / 0 |
| 210 | 210.02 | 34.46 | 0 / 0 |
| 240 | 239.99 | 39.84 | 0 / 0 |

At successful 210/240, NVENC-call p95 was 0.264/0.296 ms, acquire p95
0.075/0.090 ms and queue-residence p95 0.016/0.018 ms. Thus there is no established
fixed 187 FPS ceiling. All ten probe saves decoded successfully and all probe
engine processes exited. Only the original installed engine remained afterward.

Concurrent GPU sampling for the repeat:
`.cache/cs2-perf/gpu-cadence-repeat-20260912.json` (237 samples). During measured
210/240 windows graphics clocks were 1950 MHz/P0, GPU-wide graphics utilization
averaged 16/18%, and **aggregate** encoder utilization averaged 84/93%.
Those percentages include the released recorder and cannot establish the
capacity/headroom of one Clipture instance. The first failing matrix did not
have equivalent synchronized GPU sampling; the later 20-second sample
`gpu-followup-20260912.json` is not evidence of its clock state. Clock/power-state
effects remain a hypothesis, not a demonstrated cause. No power or driver
settings were modified.

#### Next improvements to investigate, in priority order

1. Run a **single-recorder**, controlled-motion comparison with the test build,
   synchronized GPU sampling and a fresh diagnostics export. Closing the
   installed recorder would discard its replay window, so it was left
   running. Multiple recorder sessions are a material confound at
   210/240 and should be removed before selecting another production change.
2. Instrument actual GPU preparation work rather than treating CPU submission
   times as GPU execution times. A small, opt-in GPU timestamp module should
   sample copy/tonemap/cursor stages, read results later without waiting on the
   capture thread, skip unavailable samples, and reject disjoint measurements.
   Microsoft specifies paired timestamps within a disjoint-query interval to
   obtain reliable GPU elapsed time; disjoint results are invalid.
   [D3D11 query documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query).
   This instrumentation is **proposed, not implemented** in this follow-up.
3. Reconfirmed avoidable preprocessing: `selectFrameTimestamp()` currently
   accepts every acquisition; DXGI copies and tone-maps before the scheduler's
   `consumeAllAndGetLatest()` chooses a frame. The first test/120 window published
   about 245 frames/s but encoded about 120 distinct frames/s. Defer expensive
   preprocessing to selected frames only if GPU timing supports the benefit.
   Preserve cursor state at capture time, texture leases, HDR/color output,
   capture recovery and all current buffer capacities. Do not simply throttle
   acquisition or enlarge the queue. This remains an architecture experiment,
   not a silently enabled optimization.

No new production engine changes were made in this follow-up. The
performance-analysis approach produced repeatable probe tooling and evidence
for the next experiment, not a claim that CS2 at 210/240 is now stable.

### Implemented and launched: pacing and playback fixes

Fixes implemented:

- `EncoderCadence.hpp`: rational, monotonic-clock-anchored output deadlines.
  Late wakes skip expired ticks instead of issuing back-to-back catch-up ticks
  with a video clock drifting behind elapsed time. Existing duplication behavior
  remains enabled; the diagnostic now truthfully reports it. Scheduler lateness
  and skipped/repeated ticks are recorded rather than left at zero.
  Revised in 1.6.4: skipping every expired tick made clips visibly choppy in
  games, because an ordinary late wake left a gap in the output timeline. A
  wake up to 70 ms late now catches up tick by tick (each on its own grid
  timestamp, so the video clock never drifts); only longer stalls skip ahead.
- `DeadlineWait.hpp`: high-resolution waitable timer, with a short encoder spin
  tail. DXGI acquisition stays non-blocking; after an empty poll, a 0.2 ms
  requested idle wait happens **outside** DXGI. OS wakeup precision is not
  guaranteed to equal that requested duration.
- Actual encode-job enqueue timestamps now drive queue-residence measurements.
  Previously a held source frame's capture timestamp was reused, so reported
  queue delay grew while repeating static content even if the queue was empty.
  Earlier reported 784–934 ms queue values are therefore not proof of a queue
  that deep. NVENC call timing is separately measured and remains evidence of
  real submission stalls.
- UI admission keeps the total six-request ceiling but reserves two slots from
  background requests for playback setup/release. The player retries only
  explicit pre-dispatch busy errors at 100/250/500 ms, cancels pending retries
  on clip change/unmount, and ignores stale results. Timeouts are not retried.
  Generic command-registration advice was removed from error messages.

No capture/encoder/replay buffer sizes, HDR processing, bitrate, resolution,
preset, audio mixer, or save policy were changed. These targeted fixes are not
yet a verified cure for the original CS2 transition stalls or all gameplay
duplicates: a fresh CS2 run is still needed.

#### Measured alternatives, including a rejected regression

The performance-analysis approach caught and rejected an initial candidate that
used `AcquireNextFrame(1)` on the shared graphics device. It reduced CPU use, but
a repeat without compilation showed only 116.19 output FPS at 120 and 123.44 at
144, with 159 queue drops in the 144 trial. This is an observed regression;
shared-device serialization is the working explanation, not proven by GPUView.
Artifacts: `.cache/cs2-perf/cadence-probe-u5QN9m` (first run overlapped compilation)
and `cadence-probe-4YXPsh` (repeat). **That candidate was never launched for gameplay.**

The final candidate restores zero-timeout acquisition and waits outside the
graphics call. Microsoft documents zero-timeout acquisition as an immediate
availability check and recommends bounded waits when shutdown must remain
responsive. [AcquireNextFrame documentation](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-acquirenextframe).

Final sequential desktop probes, no audio, system resolution/HDR, NVENC P3,
50 Mbps, isolated workspace saves; normal recorder left running:

| Trial | Output FPS | CPU core equivalents | Reported drops | Saved video decode |
| --- | ---: | ---: | ---: | --- |
| Original at 120 | 120.026 | 1.151 | 0 | Pass |
| Final candidate at 120 | 120.039 | 0.220 | 0 | Pass |
| Final candidate at 144 | 144.038 | 0.378 | 0 | Pass |

Artifact: `.cache/cs2-perf/cadence-probe-dYQiK0/report.json`. Counter windows
lasted about 10.7 seconds after 2.5-second warmup. FPS is computed from output
packet deltas over diagnostic collection time, not the shorter CPU sampling
interval. CPU core equivalents use process CPU-seconds divided by monotonic
wall seconds (1.0 = one logical core). About 81% less CPU in this particular
120 comparison; scene/activity was not controlled, trials were sequential and
short, so do not generalize that percentage or output rate to game-load or
unique-frame guarantees. All probe processes exited. Original media untouched.

Verification: all five native CTest suites passed, including rational deadlines
at all seven supported FPS choices over two minutes of simulated time and
75 ms late-wake recovery without catch-up bursts; existing audio and replay
regressions passed. Rust admission tests passed (background saturation, reserved
playback capacity, full-cap rejection, permit recovery). TypeScript checking,
host contract, real adapter tests, player lifetime and deterministic playback
retry/cancellation tests passed. Tauri release compiled successfully.

For the restart, stopped only the verified development process
tree, staged the final runtime and relaunched normally. Windows briefly retained
an engine file lock; staging succeeded on retry before launch. Controller PID
15940, UI-worker PID 6572, engine PID 32332; startup log confirms DXGI HDR and
audio loopback initialization. Existing settings SHA256 remained
`FFB6F1F5A33D431F0BD24B8128D0B7CACFA503D729873BE2830BB010348CF437`.
Final engine SHA256 `84C02F11757687EE3BB4F2FF3875F62D9FA09F2369A6F88BBA83C2CE2359065C`;
app SHA256 `EF6672BF804CA4D8E6D36A9C32CD7182AD904D1886504713461D7DCC222C18AA`.
Installed Program Files binaries were not changed. Startup's unrelated automatic
updater warning (no endpoints in local build) remains outside this fix.

Next verification is the same CS2 120 FPS gameplay/round sequence and an immediate
diagnostic export. Check unique-frame cadence as well as output FPS, now-correct
queue residence and scheduler wake tails. The original reported stalls cannot
be marked resolved solely from these desktop probes.

Follow-up settings implementation: the UI and both hosts previously exposed or
accepted only 24/30/60, despite the native engine allowing up to 240. The earlier
instruction to select 120 was therefore premature. Added experimental
120/144/210/240 choices with matching shared types, adapter normalization,
Electron/Rust validation, contract fixtures and isolated persistence tests.
Eight Rust settings tests, both real adapter save/get paths, the host contract,
and frontend/Electron TypeScript checks pass. No capture implementation, buffers,
or user-profile settings were changed by this addition. The following initial
investigation status refers to the earlier measurement phase.

Investigation only. No capture, encoder, buffering, priority, HDR, or game
settings have been changed by this investigation. After finding the recorder
absent in Wingman A, the installed controller was launched
hidden with the local audio-mixer-fix engine for Wingman B.
Performance-analysis methodology was used to separate measured results from
source-level suspects. No extra profiling package was installed.

Symptom: CS2 itself stays smooth while Clipture loses recording smoothness,
especially in smoke. No saved example existed at the start. Private
Wingman rounds are used to make transition problems repeatable. Actual smoke and
round-transition telemetry is still needed before selecting an engine change.

## Supplied 120 FPS diagnostics: September 11 follow-up

Source: user-supplied `Clipture diagnostics 2026-09-11T18-04-16-835Z.json`
in Downloads. Analyzed read-only using raw activity deltas and the saved-clip
cadence report, following the performance-analysis methodology. No engine,
player, profile or buffer changes were made during this analysis.

The report identifies packaged 1.5.4, 2560x1440, HDR tonemapping, NVENC P3,
50 Mbps, 120 FPS target and a 210.001 Hz display. Its timeline contains three
60 FPS windows followed by 826 usable 120 FPS windows (about 27.49 minutes).
Do not average those targets together. `exportedAt` is 18:04:16.835Z but the
last timeline sample is 18:05:49.352Z; use each sample's own timestamp for
correlation, rather than assuming the filename is the end of collection.

| Selected 120 FPS windows | Seconds | Encoded FPS | Distinct-source FPS | Queue rejections |
| --- | ---: | ---: | ---: | ---: |
| All usable windows | 1649.558 | 118.56 | 80.63 | 2353 |
| Observed desktop presents >=120/sec | 769.506 | 120.04 | 110.35 | 0 |
| Observed desktop presents <20/sec | 147.367 | 102.73 | 7.21 | 2271 |

These subsets are observational, not controlled game-state labels. Roughly
96.5% of admission rejections occurred in low-present windows. Low presents
could describe menus, transitions, loss of game focus or source/capture stalls;
the JSON alone does not identify which. Window-boundary completion/drain can
produce a measured output rate slightly above the configured target.

The `lastClipCadence` analysis covers 121.505 seconds and 14359 encoded samples:
10334 distinct source frames (85.05/sec), 4025 repeated frames (28.03%), and 53
timestamp-gap events with a maximum gap of 125 ms. The first 60 one-second
buckets have 7201 samples but 6242 distinct frames (104.03/sec) and 959 repeats.
This establishes repeated source use, not a pixel-difference analysis or proof
of visible stutter throughout gameplay. A static or loading scene legitimately
needs repeated frames. Need the corresponding clip path and scene timestamps
before judging motion-specific performance or advancing the stable-FPS claim.

Largest queue-rejection windows show NVENC call p95 around 58–82 ms and queue
residence p95 around 784–934 ms, with 16–17 frames in flight and only 3–4
observed desktop presents/sec. This points to transient pipeline stalls worth
correlating with round transitions; it does not establish NVENC silicon as the
root cause. There were no reported capture access losses, slot exhaustion or
replay archive write failures in the final cumulative snapshot.

Two diagnostic caveats found in source: `stillFrameDuplicationEnabled()` returns
hard-coded false despite the enabled duplication policy and observed repeats;
the current scheduler loop does not record `recentSchedulerWakeLateness_`.
Therefore neither that false flag nor zero scheduler-lateness values can clear
duplication/scheduling as suspects. The included save-timing tail is dated
September 4 and must not be correlated with this September 11 test.

### Corresponding MP4 inspected

User supplied `C:\Users\aidav\Videos\Clipture\Counter-Strike 2\Clipture 2026-09-11 02-03-06 PM.mp4`.
Original read-only: 744009863 bytes, visible duration 120 seconds, H.264
2560x1440 and four stereo AAC tracks. FFmpeg reports average video rate 118.18
FPS with a 120 FPS nominal rate. A full video decode to a 30-frame contact sheet
completed without reported decode errors; this is not an audio verification.

Packet-copy inspection finds exactly 14359 video packets, matching the supplied
cadence analysis. The first packet PTS is -1.5083273 seconds and the last is
119.8161874 seconds with a final 0.1838126-second duration. The negative lead-in
explains why the diagnostic packet span is longer than the visible 120-second
clip. Align diagnostic bucket time to playback by subtracting 1.5083273 seconds;
do not label its first raw-packet bucket as visible second zero.

Direct decoded frames at visible 20, 62, 94 and 110 seconds, plus the contact
sheet, identify the following:

| Visible time | Observed scene | Packet timing evidence |
| --- | --- | --- |
| 0.49–29.49 s | Opening active gameplay, movement and combat in sampled frames | 3480 output samples over 29 s (120/sec), 3031 distinct source frames (104.52/sec), 449 repeats (12.90%) |
| About 62 s | Buy menu during pre-round countdown | Maximum PTS gap 125 ms at 62.308 s; 105 samples in second 62 |
| About 94 s | Pre-round freeze time, teammate in view | 75 ms gap at 94.183 s; 107 samples in second 94 |
| About 109–112 s | Death / round-loss screen, followed by next buy phase | Seconds 109/110/111 contain 82/60/68 samples; maximum gap 116.67 ms at 110.258 s |
| 99.49–106.49 s | Later active gameplay in sampled frames | 840 output samples over 7 s, 788 distinct (112.57/sec), 52 repeats |

Scene labels are based on decoded sample frames, not a claim of watching every
frame at realtime speed. Source identity comes from the matched engine cadence
metadata, not pixel hashing. Static scenes legitimately repeat; nevertheless,
the opening gameplay interval also has measurable repeated-source output.
The whole-clip 85.05 distinct-FPS average should **not** be described as the
normal combat FPS. The subjective impression that 120 looks good is compatible with
this evidence, but a stable 120-distinct-frame claim is not yet demonstrated.

Priority for further investigation: distinguish normal source sampling/cadence
repeats during gameplay from the much larger submission/completion stalls
around menus and round boundaries. No evidence here calls for larger replay
buffers or altered saving policy. The busy playback rejection remains a
separate UI-host admission problem. No production changes or capture restart
were performed while examining this clip.

Local extracted artifacts: `.cache/cs2-perf/clip-140306-overview.png` and
`clip-140306-at20.png`, `clip-140306-at62.png`, `clip-140306-at94.png`,
`clip-140306-at110.png` in that directory. No original media was modified.

### Playback busy error diagnosis

The screenshot's exact error originates at
`src-tauri/src/app/ui_process/server.rs`: all six shared request permits were
occupied when `clip_playback_url` arrived. Both Invoke commands and Media reads
compete for those same permits. The request is rejected before dispatching the
playback operation. Which six requests were active is not recorded in this
export, so thumbnail/media traffic is a candidate, not a confirmed occupant.

`ClipPlayer.tsx` displays the failure without retry; selecting the clip again
issues another request, consistent with the second attempt succeeding.
`HostCapabilityError` appends command-registration advice to every error; that
advice is misleading for this explicit busy response. This particular failure
is not evidence of a corrupt recording or a missing registered command.

Recommended fix: retain bounded concurrency, prevent background media requests
from starving interactive playback, and retry only explicit pre-dispatch busy
rejections with a small bounded delay/cancellation on clip change. Do not blindly
retry arbitrary mutations or increase capture buffers. Add a concurrency test
that saturates media work while requesting playback and a clip-change retry
cancellation test. No playback fix has been implemented in this analysis pass.

## Initial verified setup

- RTX 3080, 10,240 MiB VRAM; NVIDIA driver 610.88.
- Profile: 60 FPS, system-resolution capture (recent recordings 2560x1440),
  NVENC preset 3, automatic bitrate capped at 50 Mbps; HDR enabled.
- Current source requests asynchronous low-latency H.264 encoding, CBR, no
  B-frames, no lookahead, no spatial/temporal AQ, no multipass.
- Capture currently uses continuous DXGI acquisition, not encoder-driven
  acquisition (`kEnableEncoderDrivenDxgiCapture = false`).

## Experiment 1 — content complexity without CS2

Run: `node scripts/performance/nvenc-content-benchmark.cjs .cache/cs2-perf/nvenc-content-20260911.json`

12 trials, 360 frames each, 2560x1440, H.264 NVENC P3 / LL, CBR 50 Mbps, GOP 120,
no B-frames/lookahead/AQ/multipass, null output. Compared a flat frame with a
four-frame sequence of changing random detail. Three trials per case, alternating
order. NV12 uses noisy luma / neutral chroma; BGRA uses noisy RGB / opaque alpha.
Existing Clipture remained running. No second capture was started. CS2 was not
running during these trials (benchmark started 02:48:19 EDT; CS2 started 02:50:18).

| Input | Content | Median FPS | Range FPS |
| --- | --- | ---: | ---: |
| NV12 | Flat | 230.3 | 205.7–234.0 |
| NV12 | Changing detail | 160.4 | 159.6–161.7 |
| BGRA | Flat | 134.4 | 132.6–135.4 |
| BGRA | Changing detail | 131.9 | 130.8–137.0 |

**Finding:** scene complexity alone did not push this test below 60 FPS. A lack
of basic NVENC throughput is not established as the cause.

**Limits:** these are end-to-end throughput numbers including CPU pipes,
upload, startup and drain—not GPU encode latency or per-frame deadline success.
FFmpeg's transport differs from Clipture's shared D3D textures. There was no
CS2 graphics load or HDR compute pass. NV12/BGRA results cannot isolate conversion
cost because their CPU byte traffic differs. This does not rule out encoding
stalls under shared GPU contention.

Raw results: `.cache/cs2-perf/nvenc-content-20260911.json` (local artifact).

## Experiment 2 — unlabelled low-load GPU observation

20 seconds, 39 vendor samples, 02:50:27–02:50:47 EDT. CS2 had launched nine
seconds earlier; scene/focus was not controlled. The artifact's original
`desktop-gpu` filename must **not** be interpreted as a clean no-CS2 baseline.

| GPU-wide metric | Median | Range |
| --- | ---: | ---: |
| GPU utilization | 21% | 10–38% |
| Encoder utilization | 27% | 18–36% |
| VRAM used | 3504 MiB | 3448–3947 MiB |
| GPU temperature | 61°C | 60–62°C |

This observation shows no sustained saturation in that interval. It says
nothing about smoke bursts or sub-millisecond scheduling stalls. It includes
all applications, not just Clipture.

Raw results: `.cache/cs2-perf/desktop-gpu-20260911.json`.

## Preliminary CS2-running sample (not a marked smoke trial)

02:52:27–02:53:57 EDT, 177 samples. GPU utilization median 35%, maximum 94%
(only one sample at or above 90%); encoder median 28%, maximum 31%; VRAM
7608–7800 MiB. A local Wingman match was being set up and no smoke/round
timestamps were supplied for this window. Therefore the isolated GPU peak is
**not** evidence that a particular smoke caused saturation or a capture drop.

Raw results: `.cache/cs2-perf/cs2-live-20260911.json`. A fresh, marked trial is
required once the user is loaded into Wingman.

## Wingman A — recorder missing, not a valid capture trial

Once in the map, a 90-second sample was started
(`.cache/cs2-perf/wingman-A-20260911.json`, 176 samples). During this window both
ordinary process enumeration and an elevated CIM check found CS2 PID 40704 but
**no Clipture controller or engine**. These spot checks
do not establish recorder state for every instant, but this run cannot be used
as evidence of Clipture's capture performance or a comparison against it.

GPU utilization median 83%, peak 98%; encoder median 9%, peak 12%; VRAM
8581–8807 MiB; temperature 61–72°C. The encoder activity is GPU-wide and cannot
be assigned to Clipture when it is absent. Graphics headroom was sometimes low,
but no recorder-drop correlation is available. A repeat with verified recording
and the diagnostic export is required. No app was automatically relaunched.

## Source-level suspects, not yet proven causes

### Updated objective: sustained high-FPS recording

Later sessions gave mostly stable smoke recordings with Reflex +
Boost enabled and particle detail reduced to Low. These are game-setting
changes, not controlled independent trials: neither change can be assigned the
improvement from this evidence. The actual objective is stable **120, 144, and
210 FPS recording**, rather than only maintaining 60 FPS in smoke.

`EncoderWorker::run` clamps requested FPS to 24–240; its pacing loop has no
hard-coded 60 FPS ceiling. `EncoderPipelinePolicy.hpp` enables frame duplication,
so a nominal high output FPS does not establish the same rate of distinct source
frames. Relevant output periods are 16.67 ms at 60, 8.33 ms at 120, 6.94 ms at
144, and 4.76 ms at 210. These are cadence budgets, not a requirement that every
asynchronous encode complete within one period; sustainable throughput, queue
growth, delivery jitter, and frame freshness must be assessed together.

The pacing loop uses `sleep_for` followed by a spin tail, then selects the latest
published frame. Wakeup jitter and source/output cadence mismatch are additional
hypotheses at high rates, not measured causes. Verify actual desktop-present and
fresh-published rates before expecting 210 distinct frames/s. A high in-game FPS
counter alone does not establish what the capture backend receives.

Next controlled test: keep game settings, resolution, HDR, preset, bitrate and
buffers fixed; record the same continuously moving clear scene at 60 then 120
FPS, exporting diagnostics immediately after each. Ten uninterrupted seconds is
useful for initial localization; longer runs are required to establish stability.
Inspect distinct-source output, acceptance/completion rates, interval tails and
queue growth. Advance to 144 and 210 only after identifying the first failing
stage. Do not treat the FFmpeg benchmark as Clipture's native throughput ceiling.

### Wingman B: recorder verified at start

Launched `C:\Program Files\Clipture\clipture.exe --hidden`
with the local engine override `build/engine/audio-mixer-fix/clipture_engine.exe`.
Verified controller PID 35632, engine PID 31600 and CS2 PID 40704. Startup logs
confirmed HDR DXGI capture and CS2 detection. No profile or installed binaries
were modified. These identities describe this historical trial, not subsequent
app launches.

03:01:02–03:02:32 EDT, 177 samples, artifact
`.cache/cs2-perf/wingman-B-20260911.json`: GPU utilization median 87%, peak 100%;
80 samples at least 90%, 43 at least 95%. Encoder utilization median 29%, peak
39%. VRAM median 9230 MiB, peak 9296 of 10240 MiB. This establishes periods of
high GPU-wide load, not which stage missed a deadline. No synchronized exported
engine history or marked clip has yet been analyzed. This was not a high-FPS
recording trial. The requested 20-second clear/smoke sequence was interrupted by
bots and limited grenades, so it is not a controlled A/B comparison.

The log also records DXGI output loss `0x887A0026` at 03:21:22 EDT and a recovery
attempt. That is outside the sampled window; do not attribute it to the smoke
trial without matching timestamps and diagnostics.

1. **Excess preprocessing before selecting 60 FPS.**
   `CaptureSharedState::selectFrameTimestamp` currently accepts every update;
   it does not apply the target-FPS sampler. DXGI then copies and, with HDR,
   dispatches a full-frame tonemap before publishing. At 180 desktop updates/s,
   this can perform three times as many tonemaps as the 60 FPS output needs;
   at 240 updates/s, four times. Actual rates must come from the export, not
   merely the monitor's advertised refresh rate.
2. **Shared GPU scheduling and context pressure.**
   HDR tonemapping uses a compute shader; capture copies, cursor composition,
   and encoder input share the graphics device/context. Smooth game FPS does
   not prove these recorder tasks meet every 16.67 ms deadline. NVENC's core is
   separate from graphics, but the capture/preprocessing pipeline is not.
   [NVIDIA's encoder guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html)
   and [OBS's performance troubleshooting](https://obsproject.com/kb/encoding-performance-troubleshooting)
   explain why hardware encoding does not remove every shared-GPU dependency.
3. **Immediate-poll loop / CPU scheduling.**
   `AcquireNextFrame(0)` loops with `SwitchToThread()` on an empty poll. This is
   not an event-driven idle wait. It is a candidate to measure, not permission
   to replace it with an arbitrary sleep that might introduce capture jitter.
4. **Diagnostic smoothing can obscure the timeline.**
   Both recent capture freshness and distinct encoded-source FPS have fast
   upward response / smoothed decay. A displayed “60” is not a per-frame timing
   trace. Use raw counter deltas and saved-video cadence. Superseded high-refresh
   frames are not automatically missed 60 FPS output frames.

Relevant source: `engine/src/CaptureBackend.cpp:402`,
`engine/src/DesktopDuplicationBackend.cpp:331`, `:436`,
`engine/src/Tonemapper.cpp` (`Process`),
`engine/src/EncoderWorker.cpp:504`, `:2732`.

## Reproduction and next decision

Local Wingman: `game_type 0; game_mode 2; map de_overpass`.
[Mode mapping](https://developer.valvesoftware.com/wiki/Counter-Strike:_Global_Offensive/Game_Modes).
Keep ordinary round rules initially; distinguish a normal round reset from an
actual level load or capture-device recreation using export epoch/access-loss
counters. Do not force `mp_restartgame` for the first trial unless the original
problem specifically depends on that command.

1. Start a 90-second sample when loaded, not while compiling/loading the map:
   `node scripts/performance/sample-gpu.cjs 90 .cache/cs2-perf/wingman-A.json`
2. About 20 seconds turning slowly in a clear scene, then repeat smoke/fire
   while continuing a similar camera turn; include a natural round transition.
   Note approximate times. Keep recorder UI closed while playing.
3. Save the clip. Immediately open Clipture and **Export diagnostics** to a JSON
   file. Its bounded host history continues without a WebView (900 samples,
   normally around two seconds apart); do not restart first.
4. Analyze with:
   `node scripts/performance/summarize-frame-export.cjs <export.json> <new-summary.json>`
5. Only after baseline: repeat the same scene with a temporary lower in-game FPS
   cap that leaves GPU headroom while keeping gameplay above 60 FPS. This is a
   causal experiment, not a proposed permanent workaround. Record/restore the
   original cap. Keep all Clipture settings unchanged.

| Evidence during the marked slowdown | Interpretation / next experiment |
| --- | --- |
| Fresh capture collapses, accepted/output follow it | Acquisition/preprocessing/source delivery first |
| Fresh capture healthy, encoder acceptance drops | Submission queue / input preparation / admission |
| Acceptance healthy, output stalls and NVENC in-flight grows | Encoder completion / synchronization |
| Scheduler miss counters jump | CPU wakeup/scheduling; correlate with GPU and transitions |
| Epoch/access-loss increments at round boundary | Capture recreation path, not just steady-state encoding |
| Lower game cap restores recording, same scene/settings | Stronger evidence of shared-resource contention |

CPU timings around `CopyResource` or `Dispatch` mostly measure submission, not
GPU completion. Low CPU preparation time cannot clear the HDR pass as a suspect.
If exports plus utilization cannot localize the stall, use a short GPUView/WPR
trace or explicit GPU timestamp queries as a separate, scoped next step.

If excess HDR work is confirmed, a promising architecture experiment is to keep
fresh acquisition but defer expensive tonemapping until the chosen output frame.
Preserve texture lifetime, source provenance, cadence, and current buffer sizes.
Do **not** blindly throttle acquisition to 60 Hz or increase buffers as a first fix.

## Tool checks

Latest follow-up: [ADR 0010](adr/0010-direct-fresh-nv12-conversion-experiment.md)
tests direct fresh-frame conversion into reserved NVENC NV12 slots, without
priority increases or game changes. Compatibility validation passed; matched
CS2 freshness/game-impact validation remains pending. Early retirement is off.

Historical follow-up: [ADR 0008](adr/0008-early-source-retirement-experiment.md)
adds an opt-in private-copy / early-source-retirement experiment after the
timestamp-corrected WGC smoke trial. Current measurements, compatibility results
and candidate launch instructions are in the final sections of
[capture theory tests](cs2-capture-theory-tests.md). It is not a verified
99%-GPU smoke fix or a production default change.

Follow-up: the September 13 aligned smoke investigation and direct-source
handoff implementation are recorded in
[capture theory tests](cs2-capture-theory-tests.md#september-13-follow-up-direct-source-reader-implemented)
and [ADR 0006](adr/0006-direct-capture-texture-reader.md). The original trial
results below remain historical, not a description of the current source.

- 12 real GPU encoding trials completed, all 360 frames each.
- GPU sampler completed its 20-second and 90-second runs.
- `node scripts/performance/test-frame-export-summary.cjs` passed: weighted
  timing, reset/transition exclusions, missing-data rejection and empty history.
- Node syntax checks and `git diff --check` passed.
- No production source/configuration changes or game control performed. The
  background launch for Wingman B is documented above.
