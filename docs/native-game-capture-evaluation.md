# Native game-frame capture: evaluation

Date: 2026-11-14

Question: should Clipture add an optional capture path that takes frames from
the game's own swap chain before desktop composition (in principle like OBS
Game Capture), with WGC and Desktop Duplication kept as fallbacks?

## What composition costs

Stress window, CS2 setup (`--reflex --compute-load --render-size 1920x1440`,
12 games-priority CPU threads), RTX 3080, Windows HDR on, 2560x1440 at 210 Hz.
Presentation mode is read per frame (`IDXGISwapChainMedia::GetFrameStatisticsMedia`).

| Capture attached to the window | Presentation | Window fps lost, smoke / molly / spike |
| --- | --- | --- |
| None | hardware independent flip (overlay), 100 % | - |
| Bare WGC window session, frames discarded (`clipture_capture_floor`, FP16, 8 ms cap), 3 runs | composed, 100 % | 4.8 ± 0.2 / 2.8 / 2.4 % |
| Clipture, hybrid WGC (default), 2 runs | composed, 100 % | 9.7 / 7.3 / 4.3 % |
| Clipture, WGC game window only, 3 runs | composed, 100 % | 9.7 / 7.1 / 6.0 % |
| Clipture, WGC monitor only, 3 runs | composed, 100 % | 11.3 / 6.7 / 6.0 % |
| Clipture, DXGI Desktop Duplication, 3 runs | composed, 100 % | 13.5 / 8.1 / 5.7 % |

Every capture the OS offers takes the game off independent flip. A WGC session
that does nothing at all already costs about half of what Clipture costs. That
half is the ceiling on what capturing inside the game could win back: a hook
keeps independent flip and avoids WGC delivery, but adds its own copy of each
captured frame inside the game's frame, on the game's GPU queue.

## What a hook involves

- **APIs.** One hook per presentation path: D3D9, D3D10/11 and D3D12 through
  `IDXGISwapChain::Present`/`Present1` (and D3D12 command-queue capture),
  Vulkan through an implicit layer, OpenGL through `wglSwapBuffers`. Each needs
  its own shared-texture and synchronization path to the recorder.
- **Delivery.** Inject a module into the game process (or register a Vulkan
  layer), copy the back buffer into a shared texture at present, signal the
  recorder. HDR swap chains need their colour space handled per API.
- **Stability.** Code running inside third-party games: a crash or a hang in
  the hook is a crash or a hang of the game. Overlay-heavy titles, multi-swap
  chain engines and frame generation (presents the game does not draw) each
  need handling.
- **Anti-cheat.** Kernel anti-cheats (Vanguard, Easy Anti-Cheat, BattlEye,
  FACEIT, Ricochet) block or flag unknown modules in the game process; VAC
  bans for some injected modules. OBS's hook works with many of them because
  it is a known, signed module that vendors allow by name and signature; a new
  Clipture hook would not be. Bypassing these protections is not an option.
  Clipture's main games (CS2, Valorant, Deadlock, Overwatch, Fortnite) all
  ship such protection.
- **Licensing.** OBS's graphics hook is GPL-2.0. Clipture is MIT + Commons
  Clause, so no OBS code may be copied; a hook would be a clean-room
  implementation from Microsoft, Khronos and vendor documentation.

## Decision

Not implemented. The measured ceiling is about 4.8 points of the 9.7 Clipture
costs in a GPU-bound smoke phase, less the hook's own in-game copy. Against
it: the games Clipture is used for are exactly the ones whose anti-cheat would
reject or ban an unknown injected module, and every hook bug becomes a game
crash. WGC hybrid capture stays the default; Desktop Duplication stays the
fallback.

Worth revisiting only as an explicit per-game opt-in for titles without
anti-cheat, starting with a Vulkan implicit layer (a supported extension point
that needs no injection), and only if a release could verify per-title
compatibility.

The other half of the cost, Clipture's own pipeline on top of the WGC floor,
is what the remaining experiments target.
