# ADR 0016: Whole-monitor Windows.Graphics.Capture as the default backend

Date: 2026-10-07

Status: accepted; default in automatic mode. `CLIPTURE_CAPTURE_BACKEND=dxgi`
restores Desktop Duplication. Supersedes the automatic-mode choice in
`CaptureBackendPolicy.hpp` that preferred DXGI.

## Context

After ADR 0015, CS2 clips were smooth in normal movement but still repeated
frames inside smokes: a 60 FPS clip showed 9-28 repeats per second inside a
smoke while the game reported 150-214 FPS. A frame-selection trace
(`CLIPTURE_SELECTION_TRACE_PATH`) from a real CS2 session located the loss
upstream of the engine:

| Inside smokes | Desktop Duplication | Monitor WGC |
| --- | ---: | ---: |
| New source frames per second | 28-41 | 145-210 |
| Median gap between source present times | 21-33 ms | 5 ms |
| Longest gap | 80-117 ms | 19-43 ms |
| Encoder ticks without a new frame, per second | 20-25 | 0-7 |

With Desktop Duplication the engine picked up each desktop update within
2-4 ms; the composed desktop image itself was refreshed only ~35 times per
second. Outside smokes both backends delivered a new frame for every tick.
Game-window WGC capture also received every frame, but records the game only;
the whole desktop must stay visible, so it remains opt-in
(`CLIPTURE_EXPERIMENTAL_GAME_WINDOW_CAPTURE`).

The stress window never reproduced the desktop-refresh collapse (GPU load,
compute dispatches, stretched resolution, MMCSS "Games" CPU threads, game GPU
priority 7, game audio and foreground were all tried), so this decision rests
on the real-CS2 trace.

## Decision

Automatic mode uses whole-monitor WGC (cursor captured, capture border off,
`MinUpdateInterval` 0). If WGC fails to start or its run ends with a failure,
the output is quarantined for WGC and the session falls back to Desktop
Duplication once; a later DXGI failure stops capture instead of oscillating.
Rotated outputs keep WGC. Forced `dxgi`/`wgc` preferences are unchanged.

ADR 0015's timed catch-up applies to both backends. Tick-aligned acquisition
and the High-class acquisition thread are DXGI-only and remain in place for
the fallback.

## Results

- Real CS2, 60 FPS, smokes on de_inferno: see Context table; moving seconds in
  the clip have 0-4 repeats, and remaining empty ticks line up with the game's
  own 30-40 ms frames.
- Stress trial with the CS2-matching setup: 0 stutter frames with 40 ms and
  65 ms encoder stalls, 1 without; longest smoke/molly freeze 17 ms.
- Interleaved game-cost bench, 60 FPS, 3 rounds: smoke 15.3% (DXGI) vs 13.5%
  (WGC), molly 16.5% vs 13.2%.

## Limits

Monitor WGC copies every presented frame rather than one per encoder tick, so
it does more capture copies than tick-aligned DXGI (about 4,800 vs 2,500 per
stress run) at no measured game cost. Where the OS does not support
`IsBorderRequired(false)` the call is ignored and Windows draws its capture
border; that case does not fall back to DXGI.
