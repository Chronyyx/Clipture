# ADR 0017: Hybrid game-window capture with overlay compositing

Date: 2026-10-10

Status: accepted; on by default. `CLIPTURE_HYBRID_GAME_CAPTURE=0` restores
plain whole-monitor capture. Builds on ADR 0016.

## Context

Whole-monitor WGC (ADR 0016) fixed the ~35 desktop images per second of
Desktop Duplication, but clips still repeated frames in CS2 smokes when the
game ran uncapped above the refresh rate. In a 144 Hz session CS2's own
telemetry reported 162-175 FPS with a slowest frame of 9.5 ms, while the clip
had a new frame for only about two of every three 60 FPS ticks, and the
selection trace showed 40-55 ms gaps between desktop images. The GPU is
saturated, so the compositor misses refreshes; every whole-desktop capture
(Desktop Duplication, monitor WGC, OBS display capture) receives only the
desktop images that were composed.

Capping the game below the refresh rate (`fps_max 138`) restores headroom and
smooth clips (fast-motion repeats 7.7% -> about 0.7%), but asks every user to
tune each game. Game-only capture receives every frame but loses the desktop,
alt-tabs and Discord's overlay. Hooking the game (OBS game capture) or NVIDIA's
private frame-buffer capture were rejected: injection into anti-cheat games,
and an unlicensed, deprecated, NVIDIA-only interface.

## Decision

Monitor WGC also opens a WGC capture of the detected game window on the same
device (`WgcCaptureBackend`). Every 25 ms the backend decides which source is
published (`GameWindowCoverage`):

- The game window is used when it is foreground, covers the whole monitor,
  is not excluded from capture (`GetWindowDisplayAffinity`), its frames match
  the monitor size, and every visible window stacked above it on that monitor
  is a click-through layered window (`WS_EX_LAYERED | WS_EX_TRANSPARENT`, at
  most four). Coverage must hold for 200 ms before switching to the game.
- Anything else (alt-tab, a normal window or notification on top, Discord's
  overlay while interactive) switches back to the desktop on the next poll.
- A game window is used only after one captured frame showed content
  (`GameFrameProbe`, five 32x32 patches, sampled every 250 ms until then). A
  window that captures black only keeps the desktop. Later black scenes are
  recorded normally.

Click-through overlays are captured per window (`OverlayWindowCapture`, BGRA8,
premultiplied alpha) and drawn bottom to top over the game frame after tone
mapping (`OverlayCompositor`), as the compositor shows them over the game. The
game source waits until every overlay has delivered its first image.
Composition runs in deferred preparation, so only encoded frames pay for it.
While the game is recorded, desktop frames are released without copying.

Nothing is injected into the game process.

## Results

Real CS2, 144 Hz, uncapped, de_inferno smokes with Discord's overlay showing
the voice widget and 13 alt-tabs: fast-motion repeats 1.4% (worst second 6),
against 7.7% (worst second 19) with whole-monitor capture alone. Discord's
overlay window captured as 99.9% fully transparent with antialiased widget
edges intact.

## Limits

- Non-click-through windows above the game (Discord overlay in Shift+Tab mode,
  Windows notifications and flyouts) record the desktop, with the same losses
  under full GPU load as before.
- Overlays are drawn in SDR after tone mapping; under Windows HDR their
  brightness can differ slightly from the compositor's.
- Game cost of the extra window and overlay captures has not been measured in
  the stress trial yet.
