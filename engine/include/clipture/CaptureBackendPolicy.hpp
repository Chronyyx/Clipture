#pragma once

#include <array>
#include <string_view>

namespace clipture {

enum class CaptureBackendPreference {
    Auto,
    Dxgi,
    Wgc,
};

enum class CaptureBackendKind {
    None,
    Dxgi,
    Wgc,
    WgcWindow,
};

struct CaptureBackendDecision {
    CaptureBackendKind kind = CaptureBackendKind::None;
    bool supported = false;
    std::string_view reason;
};

inline CaptureBackendPreference parseCaptureBackendPreference(std::string_view value, bool& valid) {
    valid = true;
    if (value.empty() || value == "auto") return CaptureBackendPreference::Auto;
    if (value == "dxgi") return CaptureBackendPreference::Dxgi;
    if (value == "wgc") return CaptureBackendPreference::Wgc;
    valid = false;
    return CaptureBackendPreference::Auto;
}

inline std::string_view captureBackendPreferenceName(CaptureBackendPreference preference) {
    switch (preference) {
    case CaptureBackendPreference::Dxgi: return "dxgi";
    case CaptureBackendPreference::Wgc: return "wgc";
    default: return "auto";
    }
}

inline std::string_view captureBackendKindName(CaptureBackendKind kind) {
    switch (kind) {
    case CaptureBackendKind::Dxgi: return "DXGI Desktop Duplication";
    case CaptureBackendKind::Wgc: return "Windows.Graphics.Capture (monitor)";
    case CaptureBackendKind::WgcWindow: return "Windows.Graphics.Capture (game window)";
    default: return "none";
    }
}

inline CaptureBackendDecision decideCaptureBackend(
    CaptureBackendPreference preference,
    bool /*hdrEnabled*/,
    bool identityRotation,
    bool wgcQuarantined) {
    if (preference == CaptureBackendPreference::Wgc) {
        return { CaptureBackendKind::Wgc, true, "forced by CLIPTURE_CAPTURE_BACKEND" };
    }
    if (preference == CaptureBackendPreference::Dxgi) {
        if (!identityRotation) return { CaptureBackendKind::None, false, "forced DXGI does not support rotated output" };
        return { CaptureBackendKind::Dxgi, true, "forced by CLIPTURE_CAPTURE_BACKEND" };
    }
    if (!identityRotation) return { CaptureBackendKind::Wgc, true, "rotated output uses WGC" };
    if (wgcQuarantined) return { CaptureBackendKind::Dxgi, true, "WGC failed on this output; Desktop Duplication fallback" };
    // Desktop Duplication only sees the composed desktop, which the compositor
    // updates ~35 times/s under heavy game GPU load (CS2 smokes) while the game
    // presents 150+ FPS. Monitor WGC receives every presented frame (ADR 0016).
    return { CaptureBackendKind::Wgc, true, "default whole-monitor Windows.Graphics.Capture" };
}

inline constexpr std::array<int, 5> kDxgiRecoveryDelaysMs { 50, 100, 250, 500, 1000 };

}  // namespace clipture
