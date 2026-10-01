#include "clipture/NvencRateControl.hpp"
#include <algorithm>

namespace clipture {
NV_ENC_RC_PARAMS nvencRateControlParams(NvencRateControl mode, int bitrateMbps, uint32_t structVersion) {
    const auto cap = static_cast<uint32_t>(std::clamp(bitrateMbps, 1, 4000)) * 1'000'000u;
    NV_ENC_RC_PARAMS params{};
    params.version = structVersion;
    params.maxBitRate = cap;
    // One second of the cap: no second of footage can exceed it, which keeps
    // the replay arena and RAM budgets (sized from the cap) valid in both modes.
    params.vbvBufferSize = cap;
    params.vbvInitialDelay = cap;
    // Low latency and low GPU cost: no lookahead, multipass, adaptive
    // quantization or frame reordering in either mode.
    params.enableLookahead = 0;
    params.lookaheadDepth = 0;
    params.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    params.enableAQ = 0;
    params.enableTemporalAQ = 0;
    params.aqStrength = 0;
    params.zeroReorderDelay = 1;
    if (mode == NvencRateControl::CappedQuality) {
        params.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        params.averageBitRate = 0; // Quality-driven; the cap alone bounds it.
        params.targetQuality = kCappedQualityLevel;
        params.targetQualityLSB = 0;
    } else {
        params.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        params.averageBitRate = cap;
    }
    return params;
}

bool nvencSupportsCappedQuality(int supportedRateControlModes) {
    // Reported as a bitmask of NV_ENC_PARAMS_RC_MODE; a failed query is -1.
    return supportedRateControlModes > 0 && (supportedRateControlModes & NV_ENC_PARAMS_RC_VBR) != 0;
}

const char* nvencRateControlName(NvencRateControl mode) {
    return mode == NvencRateControl::CappedQuality ? "capped-quality VBR" : "constant bitrate";
}
} // namespace clipture
