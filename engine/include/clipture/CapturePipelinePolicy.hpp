#pragma once
#include <Windows.h>

namespace clipture {
// Process-scoped engineering overrides for one-variable A/B runs. Never persist
// into the user's settings or change queue sizes, quality, cadence or audio.
struct CapturePipelinePolicy {
    bool deferPreparation = true;
    bool nv12Input = true;
    bool isolatedDevice = true;
    bool gpuHandoff = true;
    bool idleBackoff = true;
    bool directTextureRead = true;
    bool earlySourceRetire = false;
    bool directFreshConversion = false;
};
inline const CapturePipelinePolicy& capturePipelinePolicy() {
    static const auto policy = [] {
        CapturePipelinePolicy result;
        const auto flag = [](const wchar_t* name, bool fallback) {
            wchar_t value[8] {};
            const auto count = GetEnvironmentVariableW(name, value, 8);
            if (count == 1 && value[0] == L'0') return false;
            if (count == 1 && value[0] == L'1') return true;
            return fallback;
        };
        result.deferPreparation = flag(L"CLIPTURE_DEFER_PREPARATION", result.deferPreparation);
        result.nv12Input = flag(L"CLIPTURE_NV12_INPUT", result.nv12Input);
        result.isolatedDevice = flag(L"CLIPTURE_ISOLATE_NVENC", result.isolatedDevice);
        result.gpuHandoff = flag(L"CLIPTURE_GPU_HANDOFF", result.gpuHandoff);
        result.idleBackoff = flag(L"CLIPTURE_CAPTURE_IDLE_BACKOFF", result.idleBackoff);
        result.directTextureRead = flag(L"CLIPTURE_DIRECT_TEXTURE_READ", result.directTextureRead);
        result.earlySourceRetire = flag(L"CLIPTURE_EARLY_SOURCE_RETIRE", result.earlySourceRetire);
        result.directFreshConversion = flag(L"CLIPTURE_DIRECT_FRESH_CONVERSION", result.directFreshConversion);
        return result;
    }();
    return policy;
}
} // namespace clipture
