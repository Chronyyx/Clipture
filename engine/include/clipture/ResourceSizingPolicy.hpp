#pragma once
#include <Windows.h>

namespace clipture {
// Process-scoped buffer counts. Defaults are the shipped sizes (ADR 0023); an
// override takes a small decimal from the environment for A/B runs and is never
// persisted into the user's settings.
struct ResourceSizingPolicy {
    // Buffers per Windows.Graphics.Capture frame pool (ADR 0019 option 1).
    // CLIPTURE_WGC_BUFFERS=4 restores the earlier count.
    int wgcBuffers = 3;
    // Capture slots kept warm before the pool grows on demand.
    int captureWarmSlots = 4;
    // NVENC output slots whose bitstream buffer is allocated up front; more are
    // allocated on demand. CLIPTURE_NVENC_WARM_BITSTREAMS=8 restores the earlier count.
    int nvencWarmBitstreams = 4;
    // Frames prepared before the first NVENC submission; 0 keeps preset + 1.
    int nvencSubmissionDepth = 0;
    // H.264 reference frames. The driver default (0) reserves a much larger
    // decoded picture buffer at no bitrate gain; CLIPTURE_NVENC_MAX_REFS=0 restores it.
    int nvencMaxRefs = 2;
};
inline const ResourceSizingPolicy& resourceSizingPolicy() {
    static const auto policy = [] {
        ResourceSizingPolicy result;
        const auto number = [](const wchar_t* name, int fallback, int minimum, int maximum) {
            wchar_t value[8] {};
            const auto count = GetEnvironmentVariableW(name, value, 8);
            if (count == 0 || count >= 8) return fallback;
            int parsed = 0;
            for (DWORD index = 0; index < count; ++index) {
                if (value[index] < L'0' || value[index] > L'9') return fallback;
                parsed = parsed * 10 + (value[index] - L'0');
            }
            return parsed < minimum || parsed > maximum ? fallback : parsed;
        };
        result.wgcBuffers = number(L"CLIPTURE_WGC_BUFFERS", result.wgcBuffers, 2, 8);
        result.captureWarmSlots = number(L"CLIPTURE_CAPTURE_WARM_SLOTS", result.captureWarmSlots, 1, 8);
        result.nvencWarmBitstreams = number(L"CLIPTURE_NVENC_WARM_BITSTREAMS", result.nvencWarmBitstreams, 1, 32);
        result.nvencSubmissionDepth = number(L"CLIPTURE_NVENC_SUBMISSION_DEPTH", result.nvencSubmissionDepth, 1, 16);
        result.nvencMaxRefs = number(L"CLIPTURE_NVENC_MAX_REFS", result.nvencMaxRefs, 0, 16);
        return result;
    }();
    return policy;
}
}  // namespace clipture
