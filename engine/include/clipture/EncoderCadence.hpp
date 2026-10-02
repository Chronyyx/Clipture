#pragma once

#include <algorithm>
#include <cstdint>

namespace clipture {

// Absolute rational deadlines. A wake that is only a little late (a busy game
// delaying the thread) catches up: the missed ticks are encoded back to back,
// each with its own evenly spaced timestamp, so the clip has no gap. Skipping
// them made clips visibly choppy under load. Only a long stall (a capture
// transition) skips ahead, so video timestamps cannot drift behind audio.
class EncoderCadence {
public:
    struct Tick { int64_t deadline100ns; int64_t lateness100ns; uint64_t skipped; };

    /// How far behind the schedule may fall and still be caught up.
    static constexpr int64_t defaultCatchUp100ns = 700'000; // 70 ms

    EncoderCadence(int fps, int64_t now100ns, int64_t catchUp100ns = defaultCatchUp100ns)
        : fps_(std::clamp(fps, 1, 240)), anchor_(now100ns), catchUp100ns_(std::max<int64_t>(0, catchUp100ns)) {}

    int fps() const { return fps_; }
    int64_t deadline100ns() const { return deadline(next_); }

    Tick advance(int64_t now100ns) {
        const auto lateness = std::max<int64_t>(0, now100ns - deadline(next_));
        if (lateness <= catchUp100ns_) {
            Tick result { deadline(next_), lateness, 0 };
            ++next_;
            return result;
        }
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
    int64_t catchUp100ns_;
    uint64_t next_ = 1;
};

} // namespace clipture
