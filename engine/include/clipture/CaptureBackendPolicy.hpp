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
    bool hdrEnabled,
    bool identityRotation,
    bool dxgiQuarantined) {
    if (preference == CaptureBackendPreference::Wgc) {
        return { CaptureBackendKind::Wgc, true, "forced by CLIPTURE_CAPTURE_BACKEND" };
    }
    if (preference == CaptureBackendPreference::Dxgi) {
        if (!identityRotation) return { CaptureBackendKind::None, false, "forced DXGI does not support rotated output" };
        return { CaptureBackendKind::Dxgi, true, "forced by CLIPTURE_CAPTURE_BACKEND" };
    }
    if (!identityRotation) return { CaptureBackendKind::Wgc, true, "rotated output uses WGC" };
    if (dxgiQuarantined) return { CaptureBackendKind::Wgc, true, "DXGI was quarantined after recovery failed" };
    return {
        CaptureBackendKind::Dxgi,
        true,
        hdrEnabled ? "default HDR backend" : "default SDR backend",
    };
}

inline constexpr std::array<int, 5> kDxgiRecoveryDelaysMs { 50, 100, 250, 500, 1000 };

}  // namespace clipture
