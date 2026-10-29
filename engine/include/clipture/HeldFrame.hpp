#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace clipture {

// WGC delivers every compositor frame, often 2-4x the recording rate, and each
// prepared one costs a full-frame GPU copy plus HDR tone mapping taken from the
// game. Most never reach a tick: a newer frame replaces them first.
//
// The newest frame is held unprepared. A newer frame that leaves no tick able
// to select it releases it unprepared, by FrameQueue's own release rule;
// otherwise it is prepared and queued first. The encoder flushes the held frame
// when it wakes for a tick, before selecting, so the frame is queued by the
// thread that consumes it and can never miss its tick. Ticks choose exactly the
// frames they would have chosen with every frame prepared at arrival.
//
// Not thread-safe: the capture callback and the encoder's flush share a lock.
template <typename Frame>
class HeldFrame {
public:
    // `superseded(olderPts, newerPts)` is FrameQueue::wouldSupersede.
    // `prepare(frame)` queues a frame; `release(frame)` discards one no tick
    // can select. Prepared frames go out oldest first. With no announced
    // deadline no encoder will flush, so frames are prepared at arrival.
    template <typename Superseded, typename Prepare, typename Release>
    void arrive(Frame frame, int64_t pts100ns, int64_t nextDeadline100ns,
                Superseded&& superseded, Prepare&& prepare, Release&& release) {
        if (held_) {
            if (superseded(heldPts100ns_, pts100ns)) release(take());
            else prepare(take());
        }
        if (nextDeadline100ns <= 0) {
            prepare(std::move(frame));
            return;
        }
        held_.emplace(std::move(frame));
        heldPts100ns_ = pts100ns;
    }

    // The encoder woke for a tick.
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
