#pragma once

#include "clipture/TickAlignedAcquire.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace clipture {

// WGC delivers every compositor frame, often 2-4x the recording rate, and each
// prepared one costs a full-frame GPU copy plus HDR tone mapping taken from the
// game. The newest frame is held unprepared until the encoder tick's lead
// window. A newer frame that leaves no tick able to select it releases it
// unprepared, by FrameQueue's own release rule, so ticks choose exactly the
// frames they would have chosen with every frame prepared.
//
// Not thread-safe: the capture callback and the lead-window flush share a lock.
template <typename Frame>
class HeldFrame {
public:
    // `superseded(olderPts, newerPts)` is FrameQueue::wouldSupersede.
    // `prepare` and `release` take a Frame; prepared frames go out oldest first.
    template <typename Superseded, typename Prepare, typename Release>
    void arrive(Frame frame, int64_t pts100ns, int64_t now100ns, int64_t nextDeadline100ns,
                Superseded&& superseded, Prepare&& prepare, Release&& release) {
        if (held_) {
            if (superseded(heldPts100ns_, pts100ns)) release(take());
            else prepare(take());
        }
        // Inside the lead window, past a late deadline or without a cadence,
        // a tick may select it before any flush runs.
        if (tickAlignedAcquireNotBefore(now100ns, nextDeadline100ns, kTickAlignedAcquireLead100ns) == 0) {
            prepare(std::move(frame));
            return;
        }
        held_.emplace(std::move(frame));
        heldPts100ns_ = pts100ns;
    }

    // The next tick's lead window opened.
    template <typename Prepare>
    void flush(Prepare&& prepare) {
        if (held_) prepare(take());
    }

    // The source changed or stops; the held frame can no longer be recorded.
    template <typename Release>
    void drop(Release&& release) {
        if (held_) release(take());
    }

    bool holding() const { return held_.has_value(); }

private:
    Frame take() {
        Frame frame = std::move(*held_);
        held_.reset();
        return frame;
    }

    std::optional<Frame> held_;
    int64_t heldPts100ns_ = 0;
};

}  // namespace clipture
