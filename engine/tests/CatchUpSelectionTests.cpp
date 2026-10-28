#include "clipture/CatchUpSelection.hpp"
#include "clipture/EncoderCadence.hpp"
#include "clipture/FrameQueue.hpp"
#include "clipture/HeldFrame.hpp"
#include "clipture/TickAlignedAcquire.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>

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

// Releasing superseded frames at push must not change any selection. Drive one
// queue with the tick grid announced and one without through the encoder loop's
// order (announce, wait, advance, announce, select) over jittered capture,
// 40 ms stalls and >70 ms skips, and compare every tick's choice.
void requireSupersededFramesNeverSelected() {
    for (const int fps : {30, 60, 120, 144, 240}) {
        std::mt19937 random(static_cast<unsigned>(fps));
        constexpr int64_t start = 50'000'000;
        clipture::EncoderCadence cadence(fps, start);
        clipture::FrameQueue pruned(64), reference(64);
        int64_t nextFrame = start - 30'000, lastPts = 0;
        uint64_t sequence = 0, superseded = 0;
        std::size_t prunedPeak = 0, referencePeak = 0;
        for (int iteration = 0; iteration < 4000; ++iteration) {
            pruned.announceSelectionTicks(cadence.deadline100ns(), cadence.fps());
            const auto roll = random() % 100;
            const int64_t lateness = roll < 3 ? 400'000 + random() % 100'000      // encoder stall
                : roll < 4 ? 800'000 + random() % 400'000                       // past catch-up: skip ahead
                : random() % 30'000;
            const int64_t wake = cadence.deadline100ns() + lateness;
            while (nextFrame <= wake) {
                // Present-time stamps lag arrival a little and may repeat exactly.
                const int64_t pts = std::max(lastPts, nextFrame - static_cast<int64_t>(random() % 20'000));
                lastPts = pts;
                pruned.push(frameAt(pts, ++sequence));
                reference.push(frameAt(pts, sequence));
                prunedPeak = std::max(prunedPeak, pruned.size());
                referencePeak = std::max(referencePeak, reference.size());
                nextFrame += 10'000 + random() % 60'000;  // 1-7 ms: 140-1000 Hz sources
            }
            const auto tick = cadence.advance(wake);
            pruned.announceSelectionTicks(cadence.deadline100ns(), cadence.fps());
            const bool byDeadline = clipture::selectByDeadline(cadence.deadline100ns() <= wake, tick.lateness100ns);
            const auto a = byDeadline ? pruned.consumeLatestAtOrBefore(tick.deadline100ns) : pruned.consumeAllAndGetLatest();
            const auto b = byDeadline ? reference.consumeLatestAtOrBefore(tick.deadline100ns) : reference.consumeAllAndGetLatest();
            require(a.has_value() == b.has_value() && (!a || a->sequence == b->sequence),
                    "releasing a superseded frame changed a tick's selection");
        }
        superseded = pruned.stats().coalescedDrops;
        require(superseded == reference.stats().coalescedDrops, "superseded frames must count as coalesced, not extra drops");
        require(prunedPeak < referencePeak, "stalls must hold fewer queued frames");
        std::cout << fps << " FPS: peak queued " << prunedPeak << " vs " << referencePeak << " frames.\n";
    }
}

// Holding the newest frame unprepared until the lead window, and releasing it
// when a newer one supersedes it, must select exactly what preparing every
// frame at arrival selects. Same capture, stalls and skips as above; the flush
// runs when the tick's lead window opens, before the encoder wakes.
void requireHeldFramesKeepSelections() {
    for (const int fps : {30, 60, 120, 144, 240}) {
        std::mt19937 random(static_cast<unsigned>(fps) + 7);
        constexpr int64_t start = 50'000'000;
        clipture::EncoderCadence cadence(fps, start);
        clipture::FrameQueue held(64), reference(64);
        clipture::HeldFrame<clipture::CapturedFrame> hold;
        int64_t nextFrame = start - 30'000, lastPts = 0;
        uint64_t sequence = 0, arrived = 0, prepared = 0;
        const auto superseded = [&](int64_t older, int64_t newer) { return held.wouldSupersede(older, newer); };
        const auto prepare = [&](clipture::CapturedFrame frame) { ++prepared; held.push(std::move(frame)); };
        const auto release = [](clipture::CapturedFrame) {};
        for (int iteration = 0; iteration < 4000; ++iteration) {
            held.announceSelectionTicks(cadence.deadline100ns(), cadence.fps());
            const auto roll = random() % 100;
            const int64_t lateness = roll < 3 ? 400'000 + random() % 100'000
                : roll < 4 ? 800'000 + random() % 400'000
                : random() % 30'000;
            const int64_t deadline = cadence.deadline100ns();
            const int64_t wake = deadline + lateness;
            const int64_t flushAt = deadline - clipture::kTickAlignedAcquireLead100ns;
            bool flushed = false;
            while (nextFrame <= wake) {
                if (!flushed && nextFrame >= flushAt) {
                    hold.flush(prepare);
                    flushed = true;
                }
                const int64_t pts = std::max(lastPts, nextFrame - static_cast<int64_t>(random() % 20'000));
                lastPts = pts;
                ++arrived;
                hold.arrive(frameAt(pts, ++sequence), pts, nextFrame, deadline, superseded, prepare, release);
                reference.push(frameAt(pts, sequence));
                nextFrame += 10'000 + random() % 60'000;
            }
            if (!flushed) hold.flush(prepare);
            const auto tick = cadence.advance(wake);
            held.announceSelectionTicks(cadence.deadline100ns(), cadence.fps());
            const bool byDeadline = clipture::selectByDeadline(cadence.deadline100ns() <= wake, tick.lateness100ns);
            const auto a = byDeadline ? held.consumeLatestAtOrBefore(tick.deadline100ns) : held.consumeAllAndGetLatest();
            const auto b = byDeadline ? reference.consumeLatestAtOrBefore(tick.deadline100ns) : reference.consumeAllAndGetLatest();
            require(a.has_value() == b.has_value() && (!a || a->sequence == b->sequence),
                    "holding frames until the lead window changed a tick's selection");
        }
        require(prepared < arrived, "holding must skip preparing superseded frames");
        std::cout << fps << " FPS: prepared " << prepared << " of " << arrived << " frames.\n";
    }

    // A frame arriving inside the lead window, past a late deadline or with no
    // cadence is prepared at once; one before the window waits for the flush.
    clipture::HeldFrame<int> hold;
    int prepared = 0;
    const auto never = [](int64_t, int64_t) { return false; };
    const auto count = [&](int) { ++prepared; };
    hold.arrive(1, 1, 100'000, 166'667, never, count, count);
    require(hold.holding() && prepared == 0, "frame before the lead window must be held");
    hold.arrive(2, 2, 150'000, 166'667, never, count, count);
    require(!hold.holding() && prepared == 2, "lead window must prepare the held and the new frame");
    hold.arrive(3, 3, 200'000, 166'667, never, count, count);
    hold.arrive(4, 4, 0, 0, never, count, count);
    require(!hold.holding() && prepared == 4, "late or unknown deadline must prepare at once");
    hold.arrive(5, 5, 0, 166'667, never, count, count);
    hold.drop([&](int) { --prepared; });
    require(!hold.holding() && prepared == 3, "drop must release the held frame");
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
    requireSupersededFramesNeverSelected();
    requireHeldFramesKeepSelections();
    std::cout << "Catch-up selection, superseded release, tick-aligned acquire and held-frame preparation passed.\n";
}
