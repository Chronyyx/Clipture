# ADR 0019: WGC frame pool sizing (proposed)

Date: 2026-10-22

Status: option 2 tried and rejected 2026-10-29 (see Result). Option 1 re-measured with holding in ADR 0023; option 3 narrowed to SDR games there.

## Context

After ADR 0018 the capture queue holds about six prepared slots, and the largest
remaining engine VRAM item is Windows.Graphics.Capture's own frame pools. Hybrid
capture (ADR 0017) runs two pools at once, one for the monitor and one for the
game window, each created with four buffers. Under Windows HDR at 2560x1440 each
buffer is an R16G16B16A16 frame of 28.75 MiB, so the pools hold about 230 MiB:
roughly 40% of the engine's measured ~590 MB dedicated VRAM.

Only one pool is recorded at a time. Frames from the other are drained without
copying, and the recorded pool's frames are copied into a capture slot and
closed immediately, so few buffers are ever checked out at once.

## Options

1. **Fewer buffers per pool.** One stress-window run per count (normal load,
   60 FPS, hybrid capture recording the game window) measured 697 MB engine VRAM
   with four buffers and 616 MB with three, at equal or better stability. Two
   buffers were not measured. Fewer buffers leave WGC less room when the capture
   callback is late, which can cost frames; this is a tuning step, not a design
   change.
2. **Idle the unrecorded pool at one buffer.** Resize with
   `Direct3D11CaptureFramePool::Recreate` whenever hybrid capture switches source:
   the recorded pool keeps its full count and the other drops to one, which still
   delivers the latest frame so a switch has an image ready. A game pool opened
   while the desktop is recorded starts at one. Resizes must hold the callback
   mutex so no frame is checked out during `Recreate`. Expected saving about
   86 MiB at 1440p HDR, more combined with option 1 (pools at roughly 60-90 MiB
   instead of 230 MiB).
3. **Request 8-bit frames from WGC under HDR.** Halves every buffer but hands
   tone mapping to Windows' own HDR-to-SDR conversion. Rejected unless clip
   quality is shown to match.

## Rejected: NV12 at capture

Converting prepared frames to NV12 on the capture device would shrink the six
queued slots from 14 to 5.3 MiB each, but tone mapping and overlays still need a
shared 8-bit scratch target, leaving a net saving near 40 MiB. It would add a
colour conversion pass for every captured frame (the source refresh rate, often
144-165 per second) rather than the encoded 60, taking GPU time from the game.
The encoder already converts to NV12 for NVENC.

## Validation required

Option 2 first, then option 1 on top of it, each A/B against the current default
with the stress window recording the game window (`--as-game`), at least two
alternated runs per configuration under normal load and with `--vram-mb 8600`.
Check alt-tab and overlay switches for repeated or late frames after a resize,
and confirm in a real game before changing the default.

## Result (2026-10-29)

Option 2 was implemented with `Recreate` on every recorded-source switch: the
recorded pool at four buffers, the other at one (two while ADR 0022's desktop
gap fill holds a frame). Stress window as the game, Reflex on, ADR 0021 holding
and ADR 0022 BGRA input, two alternated runs each, all with the window in
front:

| | Engine dedicated VRAM | Committed RAM | Stutter / slot drops |
| --- | ---: | ---: | --- |
| Four buffers per pool | 714-729 MB | 439 MB | 0-2 / 0 |
| Resized on switch | 834-909 MB | 439-454 MB | 0-1 / 0 |

VRAM stayed flat at the higher level for the whole run, also with a context
flush and `IDXGIDevice3::Trim` after each resize: `Recreate` does not return the
old buffers, so resizing adds memory. Reverted. Any pool sizing must be decided
when a pool is created (option 1), not by recreating a live pool.

