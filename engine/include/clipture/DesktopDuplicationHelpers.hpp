#pragma once

#include <dxgi1_2.h>

#include <algorithm>
#include <cstdint>

namespace clipture {

inline int64_t dxgiEffectiveTimestampTicks(const DXGI_OUTDUPL_FRAME_INFO& frameInfo) {
    return std::max(frameInfo.LastPresentTime.QuadPart, frameInfo.LastMouseUpdateTime.QuadPart);
}

inline uint64_t dxgiAccumulatedFramesBeyondFirst(UINT accumulatedFrames) {
    return accumulatedFrames > 1 ? static_cast<uint64_t>(accumulatedFrames - 1) : 0;
}

inline bool captureTimestampIsStrictlyNew(int64_t previousTimestamp100ns, int64_t timestamp100ns) {
    return timestamp100ns > 0 &&
        (previousTimestamp100ns <= 0 || timestamp100ns > previousTimestamp100ns);
}

inline bool dxgiCaptureFormatSupported(DXGI_FORMAT format, bool hdrEnabled) {
    return hdrEnabled
        ? format == DXGI_FORMAT_R16G16B16A16_FLOAT
        : format == DXGI_FORMAT_B8G8R8A8_UNORM;
}

}  // namespace clipture
