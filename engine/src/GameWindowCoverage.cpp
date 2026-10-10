#include "GameWindowCoverage.hpp"

#include <dwmapi.h>

namespace clipture::capture {
namespace {

constexpr std::size_t kMaxGameOverlays = 4;

bool isCloaked(HWND window) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

// Layered windows faded to zero alpha are invisible. Per-pixel-alpha layered
// windows cannot be inspected and count as visible.
bool isFullyTransparent(HWND window) {
    if ((GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_LAYERED) == 0) return false;
    BYTE alpha = 255;
    DWORD flags = 0;
    return GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0;
}

// Overlays never take input; anything that can is a real window on top.
bool isClickThroughOverlay(HWND window) {
    const auto style = GetWindowLongW(window, GWL_EXSTYLE);
    return (style & WS_EX_LAYERED) && (style & WS_EX_TRANSPARENT);
}

std::string describe(HWND window) {
    char className[96] {};
    GetClassNameA(window, className, sizeof(className));
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return std::string(className) + " pid=" + std::to_string(processId);
}

}  // namespace

GameWindowCoverage gameWindowCoverage(HWND game, HMONITOR monitor) {
    if (!game || !IsWindow(game) || !IsWindowVisible(game) || IsIconic(game)) return { false, "game window hidden" };
    if (GetAncestor(GetForegroundWindow(), GA_ROOT) != game) return { false, "game not foreground" };
    DWORD affinity = WDA_NONE;
    if (GetWindowDisplayAffinity(game, &affinity) && affinity != WDA_NONE) {
        return { false, "game window is hidden from capture" };
    }

    MONITORINFO info { sizeof(info) };
    if (!GetMonitorInfoW(monitor, &info)) return { false, "monitor unavailable" };
    const RECT screen = info.rcMonitor;
    RECT bounds {};
    if (!GetWindowRect(game, &bounds) || bounds.left > screen.left || bounds.top > screen.top ||
        bounds.right < screen.right || bounds.bottom < screen.bottom) {
        return { false, "game does not cover the monitor" };
    }

    GameWindowCoverage result { true, {}, {} };
    for (HWND above = GetWindow(game, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
        if (!IsWindowVisible(above) || isCloaked(above) || isFullyTransparent(above)) continue;
        RECT rect {};
        RECT overlap {};
        if (!GetWindowRect(above, &rect) || !IntersectRect(&overlap, &rect, &screen)) continue;
        if (!isClickThroughOverlay(above)) return { false, "window above game: " + describe(above), {} };
        if (result.overlays.size() == kMaxGameOverlays) return { false, "too many overlays above game", {} };
        result.overlays.push_back(above);
    }
    return result;
}

}  // namespace clipture::capture
