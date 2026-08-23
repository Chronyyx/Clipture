#pragma once

#include <algorithm>
#include <cstdint>

namespace clipture {

// Selects the first source frame at or after each target-rate phase boundary.
// The phase remains anchored, so refresh rates such as 144 Hz and 210 Hz do
// not alias down to 48 FPS or 52.5 FPS when targeting 60 FPS.
class FixedRateFrameSampler {
public:
    void reset() {
        fps_ = 0;
        anchorPts100ns_ = 0;
        selectedPts100ns_ = 0;
        nextTick_ = 0;
    }

    int64_t selectedPts100ns() const {
        return selectedPts100ns_;
    }

    bool shouldSample(int64_t pts100ns, int fps) {
        const int boundedFps = std::clamp(fps, 1, 240);
        if (fps_ != boundedFps || anchorPts100ns_ <= 0 || pts100ns < anchorPts100ns_) {
            fps_ = boundedFps;
            anchorPts100ns_ = pts100ns;
            selectedPts100ns_ = pts100ns;
            nextTick_ = 1;
            return true;
        }

        if (pts100ns < targetPts(nextTick_)) return false;

        const uint64_t elapsed100ns = static_cast<uint64_t>(pts100ns - anchorPts100ns_);
        const uint64_t wholeSeconds = elapsed100ns / kTicksPerSecond100ns;
        const uint64_t partial100ns = elapsed100ns % kTicksPerSecond100ns;
        const uint64_t elapsedTicks = wholeSeconds * static_cast<uint64_t>(fps_) +
            partial100ns * static_cast<uint64_t>(fps_) / kTicksPerSecond100ns;
        const uint64_t selectedTick = std::max(nextTick_, elapsedTicks);
        selectedPts100ns_ = targetPts(selectedTick);
        nextTick_ = selectedTick + 1;
        return true;
    }

private:
    int64_t targetPts(uint64_t tick) const {
        const uint64_t wholeSeconds = tick / static_cast<uint64_t>(fps_);
        const uint64_t partialTicks = tick % static_cast<uint64_t>(fps_);
        const uint64_t offset100ns = wholeSeconds * kTicksPerSecond100ns +
            partialTicks * kTicksPerSecond100ns / static_cast<uint64_t>(fps_);
        return anchorPts100ns_ + static_cast<int64_t>(offset100ns);
    }

    static constexpr uint64_t kTicksPerSecond100ns = 10'000'000ULL;
    int fps_ = 0;
    int64_t anchorPts100ns_ = 0;
    int64_t selectedPts100ns_ = 0;
    uint64_t nextTick_ = 0;
};

}  // namespace clipture
