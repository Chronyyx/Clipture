#pragma once

#include <algorithm>
#include <cstdint>

namespace clipture {

// Absolute rational deadlines: a late wake skips expired ticks instead of
// bursting repeated frames or letting video timestamps drift behind audio.
class EncoderCadence {
public:
    struct Tick { int64_t deadline100ns; int64_t lateness100ns; uint64_t skipped; };

    EncoderCadence(int fps, int64_t now100ns)
        : fps_(std::clamp(fps, 1, 240)), anchor_(now100ns) {}

    int fps() const { return fps_; }
    int64_t deadline100ns() const { return deadline(next_); }

    Tick advance(int64_t now100ns) {
        const auto elapsed = static_cast<uint64_t>(std::max<int64_t>(0, now100ns - anchor_));
        // Inverse of floor(tick * timebase / fps), including rounded boundaries.
        const auto elapsedTicks = elapsed / timebase * fps_ +
            (((elapsed % timebase + 1) * fps_ - 1) / timebase);
        const auto selected = std::max(next_, elapsedTicks);
        Tick result { deadline(selected), std::max<int64_t>(0, now100ns - deadline(next_)), selected - next_ };
        next_ = selected + 1;
        return result;
    }

private:
    int64_t deadline(uint64_t tick) const {
        return anchor_ + static_cast<int64_t>(tick / fps_ * timebase + tick % fps_ * timebase / fps_);
    }
    static constexpr uint64_t timebase = 10'000'000;
    int fps_;
    int64_t anchor_;
    uint64_t next_ = 1;
};

} // namespace clipture
