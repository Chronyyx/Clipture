#pragma once
#include <ffnvcodec/nvEncodeAPI.h>
#include <cstdint>

namespace clipture {
// How NVENC spends bits. Capped quality is the industry-standard recording
// mode (x264 "capped CRF", NVENC "CQ VBR"): every frame aims for the same
// visual quality, simple scenes take few bits, and a one-second VBV at the
// configured bitrate bounds the busiest scenes exactly like constant bitrate
// does. Constant bitrate is kept as the fallback for encoders that refuse it.
enum class NvencRateControl { CappedQuality, ConstantBitrate };

// H.264 CQ level. Measured on an RTX 3080 with 2560x1440 120 FPS footage
// against constant bitrate at the same 50 Mbps cap: 24-32% of the bytes with
// VMAF within 0.15, and under 1% more encoder time. Spatial AQ was measured
// too and left off: larger files, lower VMAF and about 3% more encoder time,
// and game performance comes first (docs/cs2-capture-performance-investigation.md).
inline constexpr uint8_t kCappedQualityLevel = 21;

NV_ENC_RC_PARAMS nvencRateControlParams(NvencRateControl mode, int bitrateMbps, uint32_t structVersion);
// Whether NVENC reports VBR, which capped quality is built on.
bool nvencSupportsCappedQuality(int supportedRateControlModes);
const char* nvencRateControlName(NvencRateControl mode);
} // namespace clipture
