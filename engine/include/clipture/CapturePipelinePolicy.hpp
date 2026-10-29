#pragma once
#include <Windows.h>

namespace clipture {
// Process-scoped engineering overrides for one-variable A/B runs. Never persist
// into the user's settings or change queue sizes, quality, cadence or audio.
struct CapturePipelinePolicy {
    // Prepared at capture: queued frames hold no raw HDR copy (ADR 0018).
    bool deferPreparation = false;
    // NVENC takes the BGRA slot copy and converts colour itself. A per-frame
    // NV12 conversion pass shares the game's 3D engine and, under real game
    // load, held capture slots long enough to drop frames (ADR 0022).
    bool nv12Input = false;
    bool isolatedDevice = true;
    bool gpuHandoff = true;
    bool idleBackoff = true;
    bool directTextureRead = true;
    bool earlySourceRetire = false;
    bool directFreshConversion = false;
    bool tickAlignedAcquire = true;
    bool raisedGpuPriority = true;
    bool highPriorityCapture = true;
    bool timedCatchUp = true;
    bool hybridGameCapture = true;
    // WGC frames wait unprepared until the encoder's tick (HeldFrame.hpp).
    bool holdUntilTick = true;
    // While the game window is recorded, a tick with no game-window frame takes
    // the newest desktop frame, which WGC still delivers (ADR 0022).
    bool desktopGapFill = false;
    // HDR: the tone-map pass reads the WGC surface in place instead of copying
    // it into an FP16 staging texture first (ADR 0023).
    bool wgcDirectTonemap = true;
    // BGRA input: the encoder copies the capture slot straight into its NVENC
    // input surface; no intermediate canonical copy (ADR 0023).
    bool skipCanonicalCopy = true;
    // Hybrid capture: the source not being recorded is left undrained, so WGC
    // stops filling its pool until the next switch (ADR 0023). Desktop gap fill
    // still takes the newest desktop frame while the game is recorded.
    bool idlePoolNoDrain = true;
    // Experimental: under Windows HDR the game window is captured as 8-bit BGRA,
    // with no tone mapping. Clips HDR games' highlights; no HDR detection.
    bool sdrGameBgra = false;
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
        result.tickAlignedAcquire = flag(L"CLIPTURE_TICK_ALIGNED_ACQUIRE", result.tickAlignedAcquire);
        result.raisedGpuPriority = flag(L"CLIPTURE_RAISED_GPU_PRIORITY", result.raisedGpuPriority);
        result.highPriorityCapture = flag(L"CLIPTURE_HIGH_PRIORITY_CAPTURE", result.highPriorityCapture);
        result.timedCatchUp = flag(L"CLIPTURE_TIMED_CATCH_UP", result.timedCatchUp);
        result.hybridGameCapture = flag(L"CLIPTURE_HYBRID_GAME_CAPTURE", result.hybridGameCapture);
        result.holdUntilTick = flag(L"CLIPTURE_HOLD_UNTIL_TICK", result.holdUntilTick);
        result.desktopGapFill = flag(L"CLIPTURE_DESKTOP_GAP_FILL", result.desktopGapFill);
        result.wgcDirectTonemap = flag(L"CLIPTURE_WGC_DIRECT_TONEMAP", result.wgcDirectTonemap);
        result.skipCanonicalCopy = flag(L"CLIPTURE_SKIP_CANONICAL", result.skipCanonicalCopy);
        result.idlePoolNoDrain = flag(L"CLIPTURE_IDLE_POOL_NO_DRAIN", result.idlePoolNoDrain);
        result.sdrGameBgra = flag(L"CLIPTURE_SDR_GAME_BGRA", result.sdrGameBgra);
        return result;
    }();
    return policy;
}
} // namespace clipture
