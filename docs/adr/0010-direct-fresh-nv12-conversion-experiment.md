# ADR 0010: Direct fresh-frame NV12 conversion (experiment)

Date: 2026-09-19 (local; validation/launch completed September 20 UTC)

Status: opt-in candidate; compatibility passed; matched CS2 performance unproven.

## Constraint and hypothesis

Game performance takes precedence over recording. Do not increase GPU priority,
cap game FPS, change game quality, or reduce recording quality to improve this
experiment's results. An initially considered priority experiment was discarded
before a runtime trial; the encoder's existing relative priority remains +1.

The isolated WGC/NVENC pipeline already defers preparation until frame selection.
Its normal NV12 path converts the selected source into a canonical texture, then
copies that texture into a reserved NVENC input. Under GPU contention, removing
this extra copy might reduce work and input retirement delay. This is a hypothesis,
not proof that the copy caused the smoke stalls.

Subsequent real-CS2 early-retirement/frame-ready-fence
trials regressed and were rolled back. Those reports are not a newly analyzed
matched dataset here. This candidate explicitly disables early retirement and
does not resurrect a producer wait or frame-ready handshake.

## Decision and safety boundaries

`CLIPTURE_DIRECT_FRESH_CONVERSION=1` enables a default-off experiment:

1. Reserve an available, unmapped encoder slot using the existing ownership path.
2. For a new source identity, convert directly into that slot's NV12 texture.
3. Keep the existing capture-ready/consumer-complete fence lifecycle and NVENC
   registration, mapping, completion, and slot retirement rules.
4. For repetitions, lazily populate the independent canonical cache and use the
   existing copy path. Never read a mapped/in-flight NVENC input to populate it.

The first repeat can require an additional conversion compared with control.
Heavy repetition can therefore erase the benefit or regress performance. This
tradeoff must be measured; fewer copies alone is not an acceptance criterion.
Canonical allocation remains available, so this is not a claimed VRAM reduction.

Routing lives in `FreshConversionPolicy.hpp`; creation of output views for existing
NV12 input textures lives in `Nv12ConversionTarget.hpp`. `EncoderWorker.cpp` retains
resource ownership and calls these small helpers. View generation is checked on
reuse. BGRA/shared-device fallback paths retain normal preparation. The experiment
is mutually exclusive with early retirement.

No source sequence, timestamp, rational cadence, audio, queue capacity, capture
pool size, bitrate, preset, or save behavior changes. A 120 packet/s stream is not
120 distinct images/s. Sustained 120 distinct output still requires sufficiently
frequent source delivery; game HUD FPS above 120 alone does not establish this.

API basis: [VideoProcessorBlt](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videocontext-videoprocessorblt)
writes into the supplied output view; retain the input/output lifetime rules in
the [NVENC programming guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/).
Video processing timestamp limitations mean near-zero conversion query results
are not evidence that conversion is free.

## Verification completed

- Release engine build passed.
- 16/16 native tests passed, none skipped, including rational cadence, GPU fences,
  HDR/audio/replay coverage, new routing tests, and real-GPU byte equality of both
  NV12 planes for canonical-plus-copy versus direct conversion across 12 images.
- Host contract suite passed. New Node scripts and PowerShell launcher syntax passed.
- Six single-recorder hardware compatibility probes passed with clean decode,
  zero measured slot/queue/scheduler drops, and unchanged GPU priority.

| Probe | Output packets/s | Distinct sources/s |
| --- | ---: | ---: |
| WGC control | 120.034 | 61.168 |
| WGC candidate | 119.957 | 82.726 |
| WGC control repeat | 119.975 | 67.230 |
| Scaled candidate | 119.992 | 66.940 |
| BGRA fallback | 119.960 | 59.980 |
| Shared-device fallback | 119.954 | 59.977 |

These are uncontrolled desktop compatibility checks, **not a CS2 improvement
measurement**. They used P3/50 Mbps/no audio; the interactive candidate preserves
the daily P3/audio settings, at 120 FPS. The saved bitrate field is 40 Mbps but
auto-bitrate is enabled with a 50 Mbps cap; the real export reports **50 Mbps**.
Do not compare their distinct
rates as an A/B effect size.

Report: `.cache/cs2-perf/fresh-conversion-AqText/report.json`.
Engine SHA-256: `419da78c6664fa2d3a2bb149ac3dadc8725331f20a2f55ee79466623797d7966`.
Candidate stage: `.cache/cs2-perf/fresh-conversion-app-VLBWD7`.
The installed recorder was closed with permission; candidate controller 33872 and
engine 29460 launched at 02:33:20 UTC September 20. Startup confirms direct fresh
conversion, isolated encoder, and GPU priority +1. Only one recorder was present.

## Controlled game trial and rollback

The stage contains unchanged installed host/FFmpeg plus the tested engine and a
private settings copy. Saves go into `profile/clips` inside the stage. Installed
binaries and the daily settings/startup registration are not changed.

After exiting the recorder from its tray (clears unsaved replay):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/performance/start-fresh-conversion.ps1 -Stage .cache/cs2-perf/fresh-conversion-app-VLBWD7 -Mode candidate
# Same binary/profile and engineering flags, differing only in the experiment:
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/performance/start-fresh-conversion.ps1 -Stage .cache/cs2-perf/fresh-conversion-app-VLBWD7 -Mode control
```

The launcher validates hashes and passing report, refuses another active recorder,
and keeps flags process-local. Do not launch both commands together.

Run matched clear (20 s) / smoke (20 s) / clear (20 s) while slowly turning, then
save and export diagnostics promptly. Collect UTC-aligned `sample-gpu.cjs` data,
trace, clip, source/accepted/distinct rates, slot drops, NVENC tails, queue residence,
and game FPS/frametime evidence. Repeat control/candidate/control with identical
scene and settings; exclude transitions and static/menu periods. Vendor-wide
utilization is not game frametime evidence.

Reject the candidate if game performance worsens beyond run-to-run variation,
recording regresses, or correctness fails. Promote only with repeatable freshness
benefit and no measurable game penalty. User smoke testing and this acceptance
decision remain pending. Default-off is the code rollback; launching normal
installed Clipture restores the daily runtime/profile.

## First user candidate run: September 20, 02:40–02:43 UTC

Evidence:

- Export: `C:\Users\aidav\Downloads\Clipture diagnostics 2026-09-20T02-42-46-740Z.json`.
- Clip: stage `profile/clips/Counter-Strike 2/Clipture 2026-09-19 10-42-26 PM.mp4`.
- GPU samples: stage `candidate-smoke-gpu-01.json`, 353 samples from
  02:40:47.575 to 02:43:47.622 UTC. Vendor-wide GPU median 86%, maximum 99% over
  the entire sampling interval (including post-save/UI time).
- Export summary: stage `candidate-smoke-summary-01.json`. Its whole-history
  averages include waiting/static periods; do not present those as smoke FPS.

Ten-second telemetry bins from 02:40:50 through 02:42:29 held ~120 packets/s,
with zero capture-slot exhaustion, encoder queue drops, or scheduler misses.
Distinct output ranged ~82.8–107.8/s; publication ranged ~102.2–139.1/s.
Maximum sampled-window NVENC-call p95 in these bins was ~0.39 ms; queue residence
p95 max ~0.03 ms. These are maxima of window percentiles, not a pooled percentile.
The 02:42:10 bin had GPU median 98%, publication ~129.7/s, distinct ~99.8/s, and
NVENC-call p95 max ~0.29 ms. Smoke boundaries are not independently annotated.

The prior long submission/retirement stall did not appear in this measured
interval, but 120 distinct output was not reached. A matched control is still
required to attribute any improvement to direct conversion. Nonuniform source
arrival versus rational sampling remains a possible explanation for repeats even
when average source publication exceeds 120/s; this is not established causality.
Subjectively it felt better, with a slight distinct-frame improvement, not a
large change. No objective game-frametime comparison was collected. Keep the
experiment default-off pending control, and do not claim a proven smoke fix.
