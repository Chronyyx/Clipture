#pragma once

#include <algorithm>
#include <cstdint>

namespace clipture {

// Chooses the HDR-to-SDR tone curve from the peak the tone-map pass measures,
// in units of SDR white. SDR content on an HDR desktop never exceeds SDR white,
// so it maps back exactly (knee 1); once a highlight above it appears, highlights
// roll off from 75% of SDR white instead of clipping. The knee eases between the
// two, quickly towards highlights and slowly back, after a hold without any.
class ToneCurveControl {
public:
    static constexpr float kSdrKnee = 1.0f;
    static constexpr float kHdrKnee = 0.75f;
    // Above SDR white by more than FP16 rounding of scRGB SDR content.
    static constexpr float kHdrPeak = 1.02f;
    static constexpr int64_t kHdrHold100ns = 50'000'000;  // 5 s
    static constexpr int64_t kToHdr100ns = 1'500'000;     // 150 ms
    static constexpr int64_t kToSdr100ns = 10'000'000;    // 1 s

    void observePeak(float peak, int64_t now100ns) {
        if (!(peak > kHdrPeak)) return;  // also rejects NaN
        lastHdr100ns_ = now100ns;
        seenHdr_ = true;
    }

    bool hdrContent(int64_t now100ns) const {
        return seenHdr_ && now100ns - lastHdr100ns_ < kHdrHold100ns;
    }

    // Advances the ramp to `now100ns` and returns the knee to use.
    float knee(int64_t now100ns) {
        const bool hdr = hdrContent(now100ns);
        const int64_t elapsed = lastStep100ns_ ? std::max<int64_t>(0, now100ns - lastStep100ns_) : 0;
        lastStep100ns_ = now100ns;
        const float target = hdr ? kHdrKnee : kSdrKnee;
        const float step = (kSdrKnee - kHdrKnee) * static_cast<float>(elapsed) /
            static_cast<float>(hdr ? kToHdr100ns : kToSdr100ns);
        knee_ = knee_ > target ? std::max(target, knee_ - step) : std::min(target, knee_ + step);
        return knee_;
    }

private:
    float knee_ = kSdrKnee;
    int64_t lastHdr100ns_ = 0;
    int64_t lastStep100ns_ = 0;
    bool seenHdr_ = false;
};

}  // namespace clipture
