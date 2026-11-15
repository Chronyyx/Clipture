#pragma once
#include "clipture/GpuStarvationDetector.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace clipture {

// When the engine's GPU work runs at realtime (ADR 0029, ADR 0033). Pure
// state, fed one detection pass at a time (about every 2 s); GpuPriorityControl
// owns the platform calls, the boost setting and the gates that delay a raise.
//
// Rules, in order:
// - A raise is always for the game in front. A remembered game raises when it
//   comes to the front, not while it merely runs: a remembered game left in
//   the background must not make another game, or the desktop, pay for
//   realtime.
// - A raised game that stays out of the front for kAwayPasses passes is
//   stepped down; its session stays armed and raises again, without waiting
//   for drops, the moment it is back in front. Short alt-tabs keep the raise.
// - A raise that does not help is undone: after kEffectPasses passes at
//   realtime, if recording still loses at least kLossyShare of its frames and
//   at least half as many as at normal, the GPU was not what starved it
//   (CPU contention, a full disk). That game session is not raised again.
// - The rest is ADR 0029 unchanged: queued or steady drops raise; closing the
//   game, or 60 s of a quiet GPU queue (once per session), steps down.
class GpuRaisePolicy {
public:
    struct Pass {
        std::string foreground;            // lowercase; empty when no game is in front
        std::vector<std::string> running;  // lowercase running games
        uint64_t droppedTotal = 0;
        std::optional<double> inFlight;
        int64_t now100ns = 0;
        int fps = 60;
        bool realtimeInEffect = false;     // a raise may be held at normal by the gates
    };
    enum class Action { None, Raise, Lower };
    struct Decision {
        Action action = Action::None;
        std::string game;
        const char* reason = "";
        bool rememberGame = false;  // the raised game earned a place in the memory
    };
    using Remembered = std::function<bool(const std::string&)>;

    static constexpr int kGoneGamePasses = 2;
    static constexpr int kAwayPasses = 3;
    static constexpr int kEffectPasses = 15;
    static constexpr int kMinimumBeforePasses = 5;
    static constexpr double kLossyShare = GpuStarvationDetector::kLossyShare;

    Decision observe(const Pass& pass, const Remembered& remembered) {
        const auto drops = dropsSince(pass);
        if (raised_) return observeRaised(pass, drops);
        return observeLowered(pass, drops, remembered);
    }

    // Engineering smoke tests: raised with no game, held until the boost changes.
    void startRaised() {
        reset();
        raised_ = true;
        reason_ = "start raised";
    }
    // Boost turned off or on: everything starts over.
    void reset() { *this = GpuRaisePolicy {}; }

    bool raised() const { return raised_; }
    const std::string& raisedGame() const { return game_; }
    const std::string& reason() const { return reason_; }
    int lossyWindows() const { return starvation_.lossyWindows(); }
    // A pass reads packets in flight only when it can act on them: while raised
    // (memory and quiet step-down), or for a game in front that dropped frames
    // since the previous pass. Below a raise the queue matters only to the
    // queued-drops rule, which needs a drop in the same pass, so a game that
    // records cleanly costs no reads (25 D3DKMT node reads per pass).
    bool needsInFlight(const std::string& foreground, uint64_t droppedTotal) const {
        if (raised_) return !game_.empty();
        return !foreground.empty() && lastAt_ && droppedTotal != lastDropped_;
    }

private:
    struct Drops {
        uint64_t dropped = 0;
        double due = 0;
        bool valid = false;
    };
    // Rolling drop rate over the last few passes (dropped / due).
    struct DropRate {
        std::array<Drops, kEffectPasses> passes {};
        int next = 0, filled = 0;
        void add(const Drops& drops) {
            if (!drops.valid) return;
            passes[next] = drops;
            next = (next + 1) % kEffectPasses;
            filled = std::min(filled + 1, kEffectPasses);
        }
        double rate() const {
            uint64_t dropped = 0;
            double due = 0;
            for (int i = 0; i < filled; ++i) { dropped += passes[i].dropped; due += passes[i].due; }
            return due > 0 ? static_cast<double>(dropped) / due : 0;
        }
        void clear() { *this = DropRate {}; }
    };

    Drops dropsSince(const Pass& pass) {
        Drops result;
        if (lastAt_ && pass.now100ns > *lastAt_ &&
            pass.now100ns - *lastAt_ <= GpuStarvationDetector::kMaximumWindow100ns) {
            result.dropped = pass.droppedTotal >= lastDropped_ ? pass.droppedTotal - lastDropped_ : pass.droppedTotal;
            result.due = std::max(1, pass.fps) * static_cast<double>(pass.now100ns - *lastAt_) / 10'000'000.0;
            result.valid = true;
        }
        lastAt_ = pass.now100ns;
        lastDropped_ = pass.droppedTotal;
        return result;
    }

    static bool contains(const std::vector<std::string>& list, const std::string& item) {
        return std::find(list.begin(), list.end(), item) != list.end();
    }

    Decision raise(const std::string& game, const char* reason) {
        raised_ = true;
        game_ = game;
        reason_ = reason;
        missing_ = 0;
        away_ = 0;
        armed_.clear();
        // The rate at normal, judged against realtime's below; a raise from
        // memory or a return to the front has no comparable history.
        beforeRate_ = before_.filled >= kMinimumBeforePasses ? std::optional<double>(before_.rate()) : std::nullopt;
        before_.clear();
        during_.clear();
        quiet_.reset();
        remember_.reset();
        return Decision { Action::Raise, game, reason };
    }

    Decision lower(const char* reason) {
        Decision decision { Action::Lower, game_, reason };
        raised_ = false;
        game_.clear();
        reason_.clear();
        tracked_.clear();
        starvation_.reset();
        before_.clear();
        during_.clear();
        return decision;
    }

    Decision observeRaised(const Pass& pass, const Drops& drops) {
        if (game_.empty()) return {};
        if (contains(pass.running, game_)) {
            missing_ = 0;
        } else if (++missing_ >= kGoneGamePasses) {
            return lower("game closed");
        }
        if (pass.foreground == game_) {
            away_ = 0;
        } else if (++away_ >= kAwayPasses) {
            const auto game = game_;
            auto decision = lower("game in background");
            armed_ = game;
            return decision;
        }
        // Held at normal: the queue and drops say nothing about realtime yet.
        if (!pass.realtimeInEffect) return {};
        Decision decision;
        if (reason_ != "remembered" && remember_.observe(pass.inFlight, pass.now100ns)) {
            decision.rememberGame = true;
            decision.game = game_;
        }
        during_.add(drops);
        if (beforeRate_ && during_.filled >= kEffectPasses) {
            const double rate = during_.rate();
            if (rate >= kLossyShare && rate >= 0.5 * *beforeRate_) {
                blocked_ = game_;
                auto lowered = lower("raise ineffective");
                lowered.rememberGame = decision.rememberGame;
                return lowered;
            }
        }
        // One quiet step-down per game session (ADR 0029).
        if (game_ != quietGame_ && quiet_.observe(pass.inFlight, pass.now100ns)) {
            quietGame_ = game_;
            auto lowered = lower("gpu quiet");
            lowered.rememberGame = decision.rememberGame;
            return lowered;
        }
        return decision;
    }

    Decision observeLowered(const Pass& pass, const Drops& drops, const Remembered& remembered) {
        // Session marks end when their game exits.
        for (auto* game : { &quietGame_, &armed_, &blocked_ }) {
            if (!game->empty() && !contains(pass.running, *game)) game->clear();
        }
        const auto& front = pass.foreground;
        const bool eligible = !front.empty() && front != quietGame_ && front != blocked_;
        if (eligible && front == armed_) return raise(front, "back in front");
        if (eligible && remembered && remembered(front)) return raise(front, "remembered");

        // Starvation is judged per game in front; drops on the desktop or after a
        // switch to another game start the history over.
        if (front != tracked_) {
            tracked_ = front;
            starvation_.reset();
            before_.clear();
        }
        if (front.empty() || front == blocked_) return {};
        before_.add(drops);
        const auto verdict = starvation_.observe(pass.droppedTotal, pass.inFlight, pass.now100ns, pass.fps);
        if (verdict == GpuStarvationDetector::Verdict::None) return {};
        return raise(front, verdict == GpuStarvationDetector::Verdict::QueuedDrops ? "queued drops" : "steady drops");
    }

    bool raised_ = false;
    std::string game_;
    std::string reason_;
    int missing_ = 0;
    int away_ = 0;
    std::string tracked_;
    // Per game session: lowered for a quiet GPU, stepped down while away, or
    // found not to benefit. Cleared when that game exits.
    std::string quietGame_;
    std::string armed_;
    std::string blocked_;
    std::optional<int64_t> lastAt_;
    uint64_t lastDropped_ = 0;
    DropRate before_;
    DropRate during_;
    std::optional<double> beforeRate_;
    GpuStarvationDetector starvation_;
    GpuQuietWatch quiet_;
    GpuRememberWatch remember_;
};

}  // namespace clipture
