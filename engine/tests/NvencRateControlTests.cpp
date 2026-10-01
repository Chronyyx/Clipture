#include "clipture/NvencRateControl.hpp"
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "Rate control test failed: " << message << '\n';
    std::exit(1);
}
}

int main() {
    using namespace clipture;
    const auto quality = nvencRateControlParams(NvencRateControl::CappedQuality, 50, 7);
    require(quality.version == 7, "struct version is the negotiated one");
    require(quality.rateControlMode == NV_ENC_PARAMS_RC_VBR, "capped quality is VBR");
    require(quality.targetQuality == kCappedQualityLevel && quality.averageBitRate == 0,
        "quality drives the bitrate, not an average target");
    require(quality.maxBitRate == 50'000'000 && quality.vbvBufferSize == 50'000'000 &&
        quality.vbvInitialDelay == 50'000'000, "one-second VBV at the cap bounds every second");
    require(quality.enableAQ == 0 && quality.enableTemporalAQ == 0 && quality.aqStrength == 0,
        "no adaptive quantization (measured: bigger files, more encoder time)");
    require(quality.enableLookahead == 0 && quality.multiPass == NV_ENC_MULTI_PASS_DISABLED &&
        quality.zeroReorderDelay == 1, "no added latency");

    const auto constant = nvencRateControlParams(NvencRateControl::ConstantBitrate, 50, 7);
    require(constant.rateControlMode == NV_ENC_PARAMS_RC_CBR && constant.averageBitRate == 50'000'000 &&
        constant.maxBitRate == 50'000'000 && constant.vbvBufferSize == 50'000'000 &&
        constant.enableAQ == 0 && constant.targetQuality == 0, "fallback matches the previous CBR setup");

    require(nvencRateControlParams(NvencRateControl::CappedQuality, 0, 7).maxBitRate == 1'000'000,
        "a non-positive bitrate is clamped, never zero (which NVENC reads as unlimited)");
    require(nvencSupportsCappedQuality(NV_ENC_PARAMS_RC_VBR | NV_ENC_PARAMS_RC_CBR), "VBR reported");
    require(!nvencSupportsCappedQuality(NV_ENC_PARAMS_RC_CBR), "VBR missing");
    require(!nvencSupportsCappedQuality(-1), "failed query");
    std::cout << "NVENC rate control passed.\n";
    return 0;
}
