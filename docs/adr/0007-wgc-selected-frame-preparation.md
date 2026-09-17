# ADR 0007: Bring WGC onto the selected-frame pipeline

Date: 2026-09-14 UTC. Status: implemented; backend selection remains experimental.

## Context and research

After ADR 0006, the matched smoke trial still delivers only 63–65 distinct
sources/s despite ~120 output packets/s and no capture-slot or encoder backlog.
The explicit producer-side GPU wait no longer executes. High GPU utilization
does not identify which scheduling or presentation boundary loses freshness.

Good recording under high utilization is possible in some workloads, but not
guaranteed without GPU time for capture and preparation. OBS documents cases
where recording lags while the game is smooth and discusses scheduling and
headroom remedies. This is evidence for testing those factors, not proof that
Clipture needs elevation or that OBS's 60 FPS experience promises 120 FPS here.
[OBS performance guidance](https://obsproject.com/kb/encoding-performance-troubleshooting).

Alternative capture delivery is worth comparing before further changes to
NVENC. WGC captures monitors/windows and supplies QPC-relative timestamps;
HDR requires preserving the floating-point input until tone mapping. Its
checked-out surfaces must not be retained after return to the frame pool.
[Microsoft capture/lifetime guidance](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture).

## Decision

Keep DXGI as the automatic monitor backend. Modernize the existing WGC backend
so a forced `CLIPTURE_CAPTURE_BACKEND=wgc` trial compares delivery APIs with
equivalent downstream processing, rather than reintroducing an eager HDR pass
on every publication and the old encoder bridge.

- `WgcFramePreparation.*` owns format/content validation and copying into leased
  owned inputs. Reject unsupported formats and undersized surfaces rather than
  publish uninitialized pixels; crop larger sources to valid content.
- Reuse once-only desktop preparation with an optional pointer compositor.
  WGC supplies its own cursor, so no second cursor pass is performed.
- Gate raw WGC source timestamps before copying: reject nonpositive, duplicate
  and backward timestamps without advancing the accepted-source watermark.
  Reset at frame-pool epoch changes. Do not synthesize fresh source identities
  from callback arrival time; output cadence can repeat the last valid source.
- HDR uses per-slot raw inputs and runs after selection. SDR goes directly to
  the owned BGRA texture. Shared BGRA outputs carry ADR 0006 completion guards.
- The callback returns WGC surfaces after copying, before deferred shader work.
  Queued work retains the tone mapper and owned textures across epoch/reset.
  Do not reset its view cache from the callback while the encoder uses it.
- Query optional `IGraphicsCaptureSession5` before requesting a zero
  `MinUpdateInterval`; log the effective value or failure and preserve old-OS
  behavior when unavailable. This removes a requested minimum interval; it
  cannot force Windows to deliver frames at a guaranteed rate.
  [Microsoft interval API](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.minupdateinterval?view=winrt-26100).

The WGC pool stays at four frames. Owned texture pool, job queue, NVENC output
slots, rational cadence, quality, audio, replay settings and host protocol are
unchanged. Process-only backend selection is not persisted to settings.

## Alternatives not silently enabled

- DXGI late-release is a valid separate experiment: Microsoft recommends
  minimizing release-to-acquire time to avoid redundant desktop updates.
  Current prompt-release behavior is retained to avoid mixing two independent
  changes in this comparison. [ReleaseFrame guidance](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-releaseframe).
- Raising GPU priority may trade game performance for recorder progress.
  Clipture already requests relative +1 for capture and encoder devices; this
  is not a reserved GPU time slice. No administrator requirement, absolute or
  realtime priority, game cap, HAGS or driver setting change is made here.
  [Priority semantics and caution](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgidevice-setgputhreadpriority).
- No game injection or anti-cheat/CS2 launch-option changes. WGC window capture
  is an existing supported backend, not an injected game hook.

## Acceptance

WARP pixel tests check WGC HDR equivalence, source reuse before deferred work,
epoch lifetime, repeat idempotence, cropped SDR, eager fallback and invalid
input rejection. Existing DXGI/cursor, rational cadence, replay/audio and
cross-device forced-stall tests remain passing.

`probe-wgc-cadence.cjs` checks sequential DXGI/WGC 120 FPS, native DXGI BGRA, traced WGC, 144 FPS,
scaling, BGRA, eager preparation and shared-device fallback, saved video decode,
mode activation, zero reported/capture-slot drops and clean EOF shutdown.
Desktop compatibility is necessary but insufficient: compare the same CS2
clear/smoke/clear scene before considering a production backend default change.
