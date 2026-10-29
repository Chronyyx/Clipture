#include "clipture/ToneCurveControl.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
static void require(bool value) { if (!value) std::abort(); }
int main() {
    using clipture::ToneCurveControl;
    constexpr int64_t ms = 10'000;
    ToneCurveControl control;
    // SDR content, including SDR white and FP16 rounding just above it, maps exactly.
    for (int64_t t = 1; t <= 2000; t += 16) {
        control.observePeak(t % 3 ? 1.0f : 1.015f, t * ms);
        require(control.knee(t * ms) == ToneCurveControl::kSdrKnee);
        require(!control.hdrContent(t * ms));
    }
    control.observePeak(std::numeric_limits<float>::quiet_NaN(), 2001 * ms);
    require(!control.hdrContent(2001 * ms));

    // One highlight above SDR white starts the roll-off, reached within 150 ms.
    control.observePeak(3.0f, 2010 * ms);
    require(control.hdrContent(2010 * ms));
    const float partway = control.knee(2010 * ms);
    require(partway < ToneCurveControl::kSdrKnee && partway > ToneCurveControl::kHdrKnee);
    require(control.knee(2160 * ms) == ToneCurveControl::kHdrKnee);

    // Highlights keep it rolling off; 5 s without any eases back over 1 s.
    control.observePeak(2.0f, 4000 * ms);
    require(control.knee(8990 * ms) == ToneCurveControl::kHdrKnee);
    require(!control.hdrContent(9000 * ms));
    const float easing = control.knee(9500 * ms);
    require(easing > ToneCurveControl::kHdrKnee && easing < ToneCurveControl::kSdrKnee);
    require(control.knee(10000 * ms) == ToneCurveControl::kSdrKnee);

    // A late readback from earlier frames still counts from when it is seen.
    control.observePeak(1.5f, 10010 * ms);
    require(control.hdrContent(10010 * ms));
    std::cout << "Tone curve maps SDR content exactly and rolls off only while HDR highlights are present.\n";
}
