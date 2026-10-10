#include "clipture/CatchUpSelection.hpp"
#include "clipture/EncoderCadence.hpp"
#include "clipture/FrameQueue.hpp"
#include "clipture/TickAlignedAcquire.hpp"

#include <cstdlib>
#include <iostream>

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

clipture::CapturedFrame frameAt(int64_t pts100ns, uint64_t sequence) {
    clipture::CapturedFrame frame;
    frame.pts100ns = pts100ns;
    frame.sequence = sequence;
    frame.captureEpoch = 1;
    return frame;
}

int main() {
    // A 40 ms late wake at 60 FPS: capture kept publishing every 4 ms. Each due
    // tick must take the newest frame at or before its own deadline, so the
    // burst replays what was on screen instead of repeating the newest frame.
    constexpr int fps = 60;
    constexpr int64_t start = 10'000'000;
    clipture::EncoderCadence cadence(fps, start);
    clipture::FrameQueue queue(32);
    const int64_t firstDeadline = cadence.deadline100ns();
    const int64_t wake = firstDeadline + 400'000;
    uint64_t sequence = 0;
    for (int64_t pts = firstDeadline - 160'000; pts <= wake; pts += 40'000) queue.push(frameAt(pts, ++sequence));

    uint64_t previous = 0;
    int ticks = 0;
    while (cadence.deadline100ns() <= wake) {
        const auto tick = cadence.advance(wake);
        const bool moreTicksDue = cadence.deadline100ns() <= wake;
        require(clipture::selectByDeadline(moreTicksDue, tick.lateness100ns), "late tick selected the newest frame");
        auto selected = queue.consumeLatestAtOrBefore(tick.deadline100ns);
        require(selected.has_value(), "due tick found no frame");
        require(selected->pts100ns <= tick.deadline100ns, "late tick took a frame from its future");
        require(tick.deadline100ns - selected->pts100ns < 40'000, "late tick skipped a newer due frame");
        require(selected->sequence > previous, "catch-up repeated a frame");
        previous = selected->sequence;
        ++ticks;
    }
    require(ticks == 3, "40 ms at 60 FPS is three due ticks");
    // Even the last tick of the burst is 6.7 ms late; frames presented after it
    // stay queued for the next tick instead of being shown early and repeated.
    require(queue.size() == 2, "frames after the last late deadline were not kept");
    const auto onTime = cadence.advance(cadence.deadline100ns());
    require(!clipture::selectByDeadline(false, onTime.lateness100ns), "on-time tick must take the newest frame");
    auto next = queue.consumeAllAndGetLatest();
    require(next && next->sequence == sequence && next->sequence > previous, "next tick did not advance to the newest frame");
    require(clipture::selectByDeadline(false, 25'000) && !clipture::selectByDeadline(false, 15'000), "2 ms lateness threshold");

    // A frame published exactly on a deadline belongs to that tick.
    queue.push(frameAt(1'000, 1));
    queue.push(frameAt(2'000, 2));
    require(queue.consumeLatestAtOrBefore(1'000)->sequence == 1, "on-deadline frame not selected");
    require(queue.consumeLatestAtOrBefore(1'500) == std::nullopt, "future frame selected early");

    // Tick-aligned acquisition: wait until the lead window, never past it.
    using clipture::tickAlignedAcquireNotBefore;
    require(tickAlignedAcquireNotBefore(0, 0, 20'000) == 0, "unknown deadline must not wait");
    require(tickAlignedAcquireNotBefore(100'000, 166'667, 20'000) == 146'667, "wait must end at the lead window");
    require(tickAlignedAcquireNotBefore(150'000, 166'667, 20'000) == 0, "inside the window acquires now");
    require(tickAlignedAcquireNotBefore(200'000, 166'667, 20'000) == 0, "late deadline acquires now");
    require(tickAlignedAcquireNotBefore(0, 9'000'000, 20'000) == clipture::kTickAlignedAcquireMaxWait100ns,
            "far deadline re-checks within the bounded wait");
    std::cout << "Catch-up selection and tick-aligned acquire passed.\n";
}
