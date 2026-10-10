#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace clipture::capture {

// Whether a game window's own frames, plus captured overlays, show exactly
// what the monitor shows: the game is foreground, covers the whole monitor and
// everything visible above it there is a click-through overlay (Discord
// overlay, FPS counters). Any other window keeps capture on the desktop.
struct GameWindowCoverage {
    bool covers = false;
    std::string reason;
    std::vector<HWND> overlays;  // bottom to top
};

GameWindowCoverage gameWindowCoverage(HWND game, HMONITOR monitor);

}  // namespace clipture::capture
