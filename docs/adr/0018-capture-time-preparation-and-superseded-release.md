# ADR 0018: Capture-time preparation and early release of superseded frames

Date: 2026-10-22

Status: implemented; 60 FPS validated in the stress window under normal and
VRAM-exhausted load. 120 FPS and real-game confirmation pending.

## Context

With a game filling VRAM (Deadlock on a 10 GB RTX 3080), the 1.7.2 engine reached
about 1.2 GB of RAM and clips froze for up to 2 s. NVIDIA backs every non-shared
D3D11 texture with an equal process-private commit; Windows fills it when it
demotes the background engine's textures to make room for the foreground game.

Three things multiplied the engine's texture footprint:

1. `Tonemapper` cached up to 16 input/output views. A view keeps its texture
   alive, so capture slots freed for idling were never released.
2. Every captured frame kept its pool slot until the next encoder tick, although
   a newer frame before that tick already made it unselectable. During encoder
   stalls the capture pool grew to 13-14 slots.
3. With deferred preparation (ADR 0005), each slot also kept a private raw HDR
   input: 28.75 MiB at 2560x1440 beside its 14 MiB prepared BGRA texture.

## Decision

1. Capture slots own their tone-mapping SRV/UAV. `Tonemapper::Process` takes
   caller-owned views and caches nothing.
2. The encoder announces its remaining selection ticks (next tick in media time,
   rate) to `FrameQueue`. On push, the newest queued frame is released when no
   remaining tick lies in `[older.pts, newer.pts)`; a 2-unit margin keeps any
   frame whose interval is uncertain. Released frames count as coalesced, as
   they would have been at the next tick. Selection is unchanged: no frame that
   any tick could choose is released.
3. `deferPreparation` defaults to off. HDR is tone-mapped (and overlays drawn)
   into the prepared slot at capture time, so slots share one HDR scratch input.
   This reverses ADR 0005's default; `CLIPTURE_DEFER_PREPARATION=1` restores it.

Raised eviction priority for capture slots was also measured and rejected: it
forced the foreground game's own resources out (its frame rate fell to 15 FPS in
an unloaded phase) without improving capture.

## Evidence

Stress window (`--as-game --compute-load --render-size 1920x1440`, 12 games-
priority CPU threads, `CLIPTURE_TEST_ENCODER_STALL=40,700`), 60 FPS capture,
HDR 2560x1440, RTX 3080. VRAM pressure: `--vram-mb 8600` in the window process.
Two runs per configuration, alternated; all include decision 1.

| VRAM full, means | Stutter | Missed | Fresh | Longest freeze | Lag p99 | Working set |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Deferred, no release | 87 | 294 | 85.1% | 1967 ms | 1850 ms | 587 MB |
| Release only | 259 | 326 | 83.3% | 517 ms | 2060 ms | 495 MB |
| Release + capture-time preparation | 59 | 60 | 97.0% | 67 ms | 51 ms | 211 MB |

Without VRAM pressure all three had 0 stutter, 0 missed and 100% fresh frames.
Engine dedicated VRAM fell from 883 MB to 591 MB and committed RAM from 706 MB to
346 MB; peak capture slots from 13-14 to 6.

`CatchUpSelectionTests` drives a releasing and a non-releasing queue through the
encoder loop's order over 20,000 jittered ticks (30-240 FPS, 40 ms stalls and
skips past catch-up) and requires identical selections and drop counts. Peak
queued frames at 60 FPS fell from 38 to 8.

## Costs and limits

Capture-time preparation runs the tone-map shader for every captured HDR frame,
including frames later released or coalesced. ADR 0005's desktop matrix showed
deferral lowering CPU use at 120 FPS (0.146 vs 0.269 cores) without a stability
difference; the cost on high-refresh sources under smoke load must be checked at
120 FPS before relying on this beyond 60 FPS. SDR capture is unaffected by
decision 3. The stress window is not a real game; confirm in Deadlock or CS2.
