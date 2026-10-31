#include "clipture/GpuStarvationDetector.hpp"
#include <cstdlib>
#include <iostream>

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

namespace {
using clipture::GpuQuietWatch;
using clipture::GpuRememberWatch;
using clipture::GpuStarvationDetector;
using Verdict = GpuStarvationDetector::Verdict;

constexpr int64_t kSecond = 10'000'000;
constexpr int64_t kPass = 2 * kSecond;

struct Feed {
    GpuStarvationDetector detector;
    uint64_t dropped = 0;
    int64_t now = 0;
    int fps;
    explicit Feed(int rate = 60) : fps(rate) { detector.observe(0, std::nullopt, 0, fps); }
    Verdict pass(uint64_t drops, std::optional<double> inFlight) {
        dropped += drops;
        now += kPass;
        return detector.observe(dropped, inFlight, now, fps);
    }
    // Returns the 1-based pass that raised, or 0.
    int run(int passes, uint64_t drops, std::optional<double> inFlight, Verdict* how = nullptr) {
        for (int index = 1; index <= passes; ++index) {
            const auto verdict = pass(drops, inFlight);
            if (verdict != Verdict::None) {
                if (how) *how = verdict;
                return index;
            }
        }
        return 0;
    }
};

// Uncapped Overwatch on a 3080 at normal: drops per 2 s pass at 60 fps.
constexpr int kOverwatch[] = { 0, 0, 0, 1, 0, 0, 2, 4, 0, 0, 4, 12, 0, 5, 1, 4, 3, 11, 0, 10, 1, 42, 41, 5, 21 };
}

int main() {
    // Queued drops: Overwatch with ~70 packets in flight raises at its second
    // close pair of drops, 16 s in (the first 14 s had one dropped frame).
    {
        Feed feed;
        int raisedAt = 0;
        Verdict how = Verdict::None;
        for (int index = 0; index < 25 && !raisedAt; ++index) {
            how = feed.pass(kOverwatch[index], 70.0);
            if (how != Verdict::None) raisedAt = index + 1;
        }
        require(raisedAt == 8 && how == Verdict::QueuedDrops, "Overwatch with a deep queue did not raise on its second pair of drops");
    }
    // Heavy starvation with a deep queue raises within two passes (4 s).
    {
        Feed feed;
        Verdict how = Verdict::None;
        require(feed.run(5, 80, 86.0, &how) == 2 && how == Verdict::QueuedDrops, "deep-queue starvation took more than two passes");
    }
    // A deep queue without drops is a GPU that copes: never raised.
    {
        Feed feed;
        require(feed.run(900, 0, 80.0) == 0, "a deep queue without drops raised");
    }
    // One hiccup (a cutscene cut, an alt-tab) is not enough, nor are hiccups
    // further apart than three passes.
    {
        Feed feed;
        require(feed.pass(30, 80.0) == Verdict::None, "one queued drop raised");
        require(feed.run(2, 0, 80.0) == 0, "clean deep passes after a hiccup raised");
        require(feed.pass(30, 80.0) == Verdict::None, "hiccups three passes apart raised");
        require(feed.run(2, 0, 80.0) == 0, "clean passes raised");
    }
    // Drops with a short queue are left to the steady rule.
    {
        Feed feed;
        require(feed.run(5, 30, 10.0) == 0, "drops with a short queue raised before the steady rule");
    }
    // Overwatch's drops without queue data still raise, on the steady rule.
    {
        Feed feed;
        int raisedAt = 0;
        Verdict how = Verdict::None;
        for (int index = 0; index < 25 && !raisedAt; ++index) {
            how = feed.pass(kOverwatch[index], std::nullopt);
            if (how != Verdict::None) raisedAt = index + 1;
        }
        require(raisedAt > 0 && how == Verdict::SteadyDrops, "Overwatch's micro-stutters did not raise on the steady rule");
    }
    // Steady rule: an odd dropped frame never raises.
    {
        Feed feed;
        require(feed.run(900, 1, 10.0) == 0, "an odd dropped frame raised");
        require(feed.run(900, 2, 10.0) == 0, "two drops per pass raised");
    }
    // Steady rule: a stutter of several seconds, even 14 s, is not starvation.
    {
        Feed feed;
        require(feed.run(3, 120, 10.0) == 0, "a 6 s stutter raised");
        require(feed.run(20, 0, 10.0) == 0, "clean play raised");
        require(feed.run(7, 120, 10.0) == 0, "a 14 s stall raised");
    }
    // Steady rule: losing frames steadily raises after ten lossy passes.
    {
        Feed feed;
        Verdict how = Verdict::None;
        require(feed.run(30, 12, 10.0, &how) == GpuStarvationDetector::kLossyWindowsToRaise && how == Verdict::SteadyDrops,
                "steady loss did not raise after ten passes");
    }
    // Low frame rates still need a handful of drops per pass.
    {
        Feed feed(24);
        require(feed.run(30, 2, 10.0) == 0, "low-rate noise raised");
    }
    // A capture restart resets the counter; it is not a burst of drops.
    {
        GpuStarvationDetector detector;
        detector.observe(5000, 80.0, 0, 60);
        require(detector.observe(2, 80.0, kPass, 60) == Verdict::None, "counter reset raised");
        require(detector.lossyWindows() == 0, "counter reset read as drops");
    }
    // Samples closer than a second wait; a gap over 10 s clears the history.
    {
        Feed feed;
        require(feed.pass(30, 80.0) == Verdict::None, "first queued drop raised");
        require(feed.detector.observe(feed.dropped + 30, 80.0, feed.now + kSecond / 2, 60) == Verdict::None,
                "short window was judged");
        feed.now += 30 * kSecond;
        feed.dropped += 30;
        require(feed.detector.observe(feed.dropped, 80.0, feed.now, 60) == Verdict::None, "long gap counted");
        require(feed.pass(30, 80.0) == Verdict::None, "history survived a long gap");
        require(feed.pass(30, 80.0) == Verdict::QueuedDrops, "history after the gap did not raise");
        feed.detector.reset();
        require(feed.detector.lossyWindows() == 0, "reset kept history");
    }
    // Remembered after two minutes of deep queue while raised, once.
    {
        GpuRememberWatch watch;
        int64_t now = 0;
        int rememberedAt = 0;
        watch.observe(70.0, now);
        for (int index = 1; index <= 120; ++index) {
            now += kPass;
            if (watch.observe(70.0, now)) {
                require(rememberedAt == 0, "remembered twice");
                rememberedAt = index;
            }
        }
        require(rememberedAt == 60, "two minutes of deep queue were not remembered at two minutes");
    }
    // A cutscene: 30 s deep, then gameplay with a short queue, is never remembered.
    {
        GpuRememberWatch watch;
        int64_t now = 0;
        watch.observe(85.0, now);
        for (int index = 1; index <= 15; ++index) require(!watch.observe(85.0, now += kPass), "a cutscene was remembered");
        for (int index = 1; index <= 300; ++index) require(!watch.observe(10.0, now += kPass), "a short queue counted");
        require(!watch.observe(std::nullopt, now += kPass), "unknown queue counted");
    }
    // Deep time adds up across breaks, and a stalled pass is credited 10 s at most.
    {
        GpuRememberWatch watch;
        int64_t now = 0;
        watch.observe(70.0, now);
        require(!watch.observe(70.0, now += 600 * kSecond), "a stalled pass was credited in full");
        int deepPasses = 0;
        bool remembered = false;
        for (int index = 0; index < 200 && !remembered; ++index) {
            const bool deep = index % 2 == 0;
            remembered = watch.observe(deep ? 70.0 : 10.0, now += kPass);
            if (deep) ++deepPasses;
        }
        require(remembered && deepPasses == 55, "deep time did not add up across short-queue passes");
        watch.reset();
        watch.observe(70.0, 0);
        require(!watch.observe(70.0, kPass), "reset kept deep time");
    }
    // The quiet watch lowers only after a full minute under 20 packets in flight.
    {
        GpuQuietWatch quiet;
        int64_t now = 0;
        for (int pass = 0; pass < 29; ++pass, now += kPass) {
            require(!quiet.observe(8.0, now), "short queue lowered before a minute");
        }
        require(quiet.observe(8.0, now + kPass), "a minute of short queue did not lower");
        quiet.reset();
        now = 0;
        for (int pass = 0; pass < 60; ++pass, now += kPass) {
            const double inFlight = pass % 20 == 19 ? 70.0 : 8.0;
            require(!quiet.observe(inFlight, now), "a queued moment did not restart the minute");
        }
        quiet.reset();
        require(!quiet.observe(std::nullopt, 0), "unknown queue lowered");
        require(!quiet.observe(8.0, kPass), "unknown queue did not restart the wait");
        require(!quiet.observe(std::nullopt, 61 * kSecond), "unknown queue lowered after a minute");
        for (int pass = 0; pass < 40; ++pass) {
            require(!quiet.observe(46.0, pass * kPass), "uncapped Overwatch's lowest queue lowered");
        }
    }
    std::cout << "GPU starvation: queued and steady drops, hiccups, resets, gaps, the remember watch and the quiet watch passed.\n";
    return 0;
}
