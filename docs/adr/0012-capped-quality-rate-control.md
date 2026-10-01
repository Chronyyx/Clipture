# ADR 0012: Capped-quality NVENC rate control

Date: 2026-10-01

Status: accepted; default for every NVENC session.

## Context

The encoder ran constant bitrate (CBR) at the configured or automatic bitrate,
with no lookahead, AQ or multipass. CBR spends the full bitrate on menus,
desktops and still scenes. A 120 s, 1440p120 clip at the 50 Mbps automatic cap
was 754 MB whatever it showed. Game performance takes precedence over recording
(ADR 0010, `docs/cs2-capture-performance-investigation.md`), so any change must
not add meaningful GPU work.

## Decision

`NvencRateControl` (`engine/src/NvencRateControl.cpp`) builds the rate-control
parameters. Each NVENC preset path tries **capped-quality VBR** first:

- `NV_ENC_PARAMS_RC_VBR`, `targetQuality = 21`, `averageBitRate = 0`. Every
  frame aims for the same quality; the bitrate follows the content.
- `maxBitRate` and `vbvBufferSize` are the configured bitrate, with a full
  initial VBV. No second of footage exceeds the cap, so the replay arena and RAM
  budgets sized from the bitrate stay valid. The settings value is therefore a
  ceiling, and the UI says so ("Maximum bitrate").
- Unchanged low-latency, low-cost setup: no B-frames, lookahead, multipass,
  spatial or temporal AQ; zero reorder delay; 2 s GOP.

If the device does not report VBR (`NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES`)
or `nvEncInitializeEncoder` rejects the configuration, the same preset path is
retried with the previous CBR configuration, unchanged. The encoder logs which
mode is active once per session (`[encoder] ... Single-pass capped-quality VBR
(CQ 21, at most N Mbps) is active.`); the in-place engine smoke test asserts it.

This is the industry-standard recording mode: x264/x265 "capped CRF" (CRF with
`vbv-maxrate`/`vbv-bufsize`), and NVENC's CQ VBR as exposed by OBS and FFmpeg.

## Evidence

RTX 3080, driver 610.88, FFmpeg `h264_nvenc` with Clipture's parameters (P3,
low-latency tune, no B-frames, 50 Mbps cap, 1 s VBV), sources cut losslessly
from a real 2560x1440 120 FPS Clipture clip. VMAF is the harmonic mean against
the lossless source, frames paired by index.

| Footage | CBR 50 Mbps | CQ 21 | CQ 21 + spatial AQ |
| --- | --- | --- | --- |
| 20 s browser/stream | 127 MB, VMAF 97.09 | 41 MB, 96.95 | 52 MB, 96.84 |
| 15 s browser/stream | 94 MB, 97.01 | 23 MB, 96.97 | 28 MB, 96.78 |

- Worst one-second bitrate: CBR 80 Mbps (it overshoots its own cap); every
  capped-quality variant tested peaked between 20 and 46 Mbps.
- GPU encode throughput, NVDEC to NVENC with frames kept in GPU memory, four
  alternating repetitions: CBR 218 FPS, CQ 21 216-217 FPS, CQ 21 + AQ 212 FPS.
- A hardware capture with the engine (`run-in-place-engine-smoke.cjs`)
  confirmed the driver accepts the configuration and in-place saves decode.

Spatial AQ was rejected: larger files, lower VMAF and about 3% more encoder time.

## Consequences

- Clips shrink most when the content is simple. Footage that needs the full cap
  (fast motion, foliage, particles) is encoded as before: the cap binds and CQ
  behaves like CBR.
- The replay buffer's size now varies with the content. Free space this leaves
  in the in-place arena is trimmed or released at save
  (`engine/src/replay/README.md`).
- Not measured: actual in-game footage under CS2 load, and banding in dark
  gradients (VMAF is weak there). If banding appears, lower the CQ level before
  considering AQ, and re-measure encoder time.
