#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace clipture {

// Decides from the engine's own dropped-frame counter (fresh frames that never
// reached the encoder) and the work packets queued on the GPU when a game
// starves recording enough to need the realtime GPU class (ADR 0029). Pure
// state; fed one sample per detection pass (about every 2 s).
//
// Two rules:
// - Queued drops (fast): the GPU queue is deep and recording dropped frames,
//   together, in 2 of the last 3 passes (4-6 s). A deep queue says a game is
//   queuing ahead; a dropped frame confirms this GPU cannot keep recording fed
//   at that depth, so it scales with hardware: a GPU that copes with deep
//   queues never drops and is never raised. In the stress window and in
//   uncapped Overwatch on a 3080, recording lost frames whenever 55 or more
//   packets stayed in flight and never below 35.
// - Steady drops (slow, for drops without a deep queue): frames lost in 10 of
//   the last 15 passes (about 20 of 30 s). A stutter of several seconds or a
//   loading screen is not starvation. The bar is set by what shows in clips:
//   uncapped Overwatch on a 3080 at normal lost 0-42 frames per 2 s window at
//   60 fps and its clips had visible micro-stutters.
class GpuStarvationDetector {
public:
    enum class Verdict { None, QueuedDrops, SteadyDrops };

    static constexpr double kDeepQueuePackets = 40.0;
    static constexpr int kQueuedPasses = 3;
    static constexpr int kQueuedDropPassesToRaise = 2;
    // A pass loses frames (steady rule) when this share of the frames due,
    // and at least kMinimumDrops, were dropped.
    static constexpr double kLossyShare = 0.025;
    static constexpr int kMinimumDrops = 3;
    static constexpr int kWindows = 15;
    static constexpr int kLossyWindowsToRaise = 10;
    static constexpr int64_t kMinimumWindow100ns = 10'000'000;   // 1 s
    static constexpr int64_t kMaximumWindow100ns = 100'000'000;  // 10 s

    // `inFlight` is the pass's mean packets in flight (nullopt if unknown).
    Verdict observe(uint64_t droppedTotal, std::optional<double> inFlight, int64_t now100ns, int fps) {
        if (!last_) {
            last_ = Sample { droppedTotal, now100ns };
            return Verdict::None;
        }
        const int64_t elapsed = now100ns - last_->at100ns;
        if (elapsed < kMinimumWindow100ns) return Verdict::None;
        // A counter that went back (capture restarted) counts from zero.
        const uint64_t dropped = droppedTotal >= last_->dropped ? droppedTotal - last_->dropped : droppedTotal;
        last_ = Sample { droppedTotal, now100ns };
        // A long gap (sleep, a stalled pass) breaks the history.
        if (elapsed > kMaximumWindow100ns) {
            clearHistory();
            return Verdict::None;
        }
        const double due = static_cast<double>(std::max(1, fps)) * static_cast<double>(elapsed) / 10'000'000.0;
        const bool lossy = dropped >= static_cast<uint64_t>(kMinimumDrops) &&
                           static_cast<double>(dropped) >= due * kLossyShare;
        lossy_[next_] = lossy;
        next_ = (next_ + 1) % kWindows;
        filled_ = std::min(filled_ + 1, kWindows);
        queuedDrops_[queuedNext_] = dropped > 0 && inFlight && *inFlight >= kDeepQueuePackets;
        queuedNext_ = (queuedNext_ + 1) % kQueuedPasses;
        if (std::count(queuedDrops_.begin(), queuedDrops_.end(), true) >= kQueuedDropPassesToRaise) {
            return Verdict::QueuedDrops;
        }
        return lossyWindows() >= kLossyWindowsToRaise ? Verdict::SteadyDrops : Verdict::None;
    }

    int lossyWindows() const {
        return static_cast<int>(std::count(lossy_.begin(), lossy_.begin() + filled_, true));
    }

    void reset() {
        last_.reset();
        clearHistory();
    }

private:
    void clearHistory() {
        lossy_.fill(false);
        next_ = 0;
        filled_ = 0;
        queuedDrops_.fill(false);
        queuedNext_ = 0;
    }

    struct Sample {
        uint64_t dropped;
        int64_t at100ns;
    };
    std::optional<Sample> last_;
    std::array<bool, kWindows> lossy_ {};
    int next_ = 0;
    int filled_ = 0;
    std::array<bool, kQueuedPasses> queuedDrops_ {};
    int queuedNext_ = 0;
};

// Decides when a raised game is remembered (ADR 0029): once the GPU queue has
// stayed deep for 2 minutes in total while raised, in one game session. A
// cutscene or one heavy moment ends and the queue shortens before that, so it
// is never remembered; gameplay that needs realtime keeps the queue deep.
class GpuRememberWatch {
public:
    static constexpr double kDeepQueuePackets = GpuStarvationDetector::kDeepQueuePackets;
    static constexpr int64_t kDeepFor100ns = 1'200'000'000;  // 2 min
    // Time credited per pass at most, so a stalled pass is not two minutes.
    static constexpr int64_t kMaximumPass100ns = 100'000'000;  // 10 s

    // True once, on the pass that completes the two minutes.
    bool observe(std::optional<double> inFlight, int64_t now100ns) {
        const auto previous = last_;
        last_ = now100ns;
        if (done_ || !previous || !inFlight || *inFlight < kDeepQueuePackets) return false;
        deep_ += std::clamp<int64_t>(now100ns - *previous, 0, kMaximumPass100ns);
        if (deep_ < kDeepFor100ns) return false;
        done_ = true;
        return true;
    }

    void reset() {
        last_.reset();
        deep_ = 0;
        done_ = false;
    }

private:
    std::optional<int64_t> last_;
    int64_t deep_ = 0;
    bool done_ = false;
};

// Decides when a raised engine can return to normal mid-game: so few work
// packets have stayed in flight on the GPU that no game can be queuing ahead
// of recording (ADR 0029). Uncapped Overwatch on a 3080 kept 46-80 in flight
// (spikes to 113, where the drops at normal were), at either class; capped at
// 141 fps it kept 2-11. Adapter busy cannot decide this: with hardware
// scheduling it read 44-68 % while the game's overlay showed 97 %.
class GpuQuietWatch {
public:
    static constexpr double kQuietPacketsInFlight = 20.0;
    static constexpr int64_t kQuietFor100ns = 600'000'000;  // 60 s

    // `inFlight` is nullopt when unknown, which restarts the wait.
    bool observe(std::optional<double> inFlight, int64_t now100ns) {
        if (!inFlight || *inFlight >= kQuietPacketsInFlight) {
            quietSince_.reset();
            return false;
        }
        if (!quietSince_) quietSince_ = now100ns;
        return now100ns - *quietSince_ >= kQuietFor100ns;
    }

    void reset() { quietSince_.reset(); }

private:
    std::optional<int64_t> quietSince_;
};

}  // namespace clipture
