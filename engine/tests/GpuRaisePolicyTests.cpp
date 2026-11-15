#include "clipture/GpuRaisePolicy.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>

namespace {
using clipture::GpuRaisePolicy;
using Action = GpuRaisePolicy::Action;

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

constexpr int64_t kPass = 20'000'000;  // 2 s

// Drives the policy like the detection loop, with realtime applied at once.
struct Session {
    GpuRaisePolicy policy;
    std::set<std::string> memory;
    uint64_t dropped = 0;
    int64_t now = 0;
    bool realtime = false;
    int reads = 0;
    GpuRaisePolicy::Decision last;

    GpuRaisePolicy::Decision pass(const std::string& front, std::vector<std::string> running,
                                  uint64_t drops, double inFlight = 10) {
        dropped += drops;
        now += kPass;
        GpuRaisePolicy::Pass input;
        input.foreground = front;
        input.running = std::move(running);
        input.droppedTotal = dropped;
        // As the engine does: the queue is read only when the policy can use it.
        input.inFlight = policy.needsInFlight(front, dropped) ? std::optional<double>(inFlight) : std::nullopt;
        if (input.inFlight) ++reads;
        input.now100ns = now;
        input.fps = 60;
        input.realtimeInEffect = realtime;
        last = policy.observe(input, [this](const std::string& game) { return memory.count(game) > 0; });
        if (last.action == Action::Raise) realtime = true;
        if (last.action == Action::Lower) realtime = false;
        if (last.rememberGame) memory.insert(last.game);
        return last;
    }
    // Passes until the policy acts; returns the 1-based pass or 0.
    int until(Action action, int passes, const std::string& front, const std::vector<std::string>& running,
              uint64_t drops, double inFlight = 10) {
        for (int index = 1; index <= passes; ++index) {
            if (pass(front, running, drops, inFlight).action == action) return index;
        }
        return 0;
    }
};

bool reasonIs(const GpuRaisePolicy::Decision& decision, const char* reason) {
    return std::strcmp(decision.reason, reason) == 0;
}

void rememberedGameInBackgroundDoesNotRaise() {
    Session session;
    session.memory.insert("deadlock.exe");
    // At the desktop, and with another game in front, the remembered game only runs.
    require(session.until(Action::Raise, 10, "", {"deadlock.exe"}, 0) == 0,
            "a remembered game in the background must not raise at the desktop");
    require(session.until(Action::Raise, 10, "cs2.exe", {"deadlock.exe", "cs2.exe"}, 0) == 0,
            "a remembered game in the background must not raise for another game");
    require(session.until(Action::Raise, 1, "deadlock.exe", {"deadlock.exe", "cs2.exe"}, 0) == 1 &&
                reasonIs(session.last, "remembered"),
            "a remembered game raises as soon as it is in front");
}

void raisedGameLeavingTheFrontStepsDownAndComesBack() {
    Session session;
    session.memory.insert("game.exe");
    require(session.until(Action::Raise, 1, "game.exe", {"game.exe"}, 0) == 1, "remembered raise");
    // A short alt-tab (two passes) keeps the raise.
    require(session.until(Action::Lower, 2, "", {"game.exe"}, 0) == 0, "a short alt-tab must keep the raise");
    require(session.until(Action::Lower, 1, "game.exe", {"game.exe"}, 0) == 0, "back before the limit");
    // Three passes away steps down.
    require(session.until(Action::Lower, 3, "other.exe", {"game.exe", "other.exe"}, 0) == 3 &&
                reasonIs(session.last, "game in background"),
            "a raised game in the background steps down after three passes");
    require(!session.policy.raised(), "lowered while away");
    require(session.until(Action::Raise, 1, "game.exe", {"game.exe", "other.exe"}, 0) == 1,
            "the game raises again when it is back in front");
}

void armedSessionWithoutMemoryRaisesOnReturn() {
    Session session;
    // Queued drops raise a game the memory does not know.
    require(session.until(Action::Raise, 4, "game.exe", {"game.exe"}, 5, 60) > 0 &&
                reasonIs(session.last, "queued drops"),
            "queued drops raise");
    require(session.until(Action::Lower, 3, "", {"game.exe"}, 0) == 3, "steps down while away");
    require(session.until(Action::Raise, 1, "game.exe", {"game.exe"}, 0) == 1 &&
                reasonIs(session.last, "back in front"),
            "a session that needed realtime raises at once when it returns, without new drops");
    // A restart of the game ends the session mark.
    require(session.until(Action::Lower, 2, "", {}, 0) == 2 && reasonIs(session.last, "game closed"), "closed");
    require(session.until(Action::Raise, 3, "game.exe", {"game.exe"}, 0) == 0,
            "a new session starts at normal and waits for drops");
}

void ineffectiveRaiseIsUndone() {
    Session session;
    // 5 % of frames lost every pass, with a shallow queue: steady drops.
    const uint64_t drops = 6;  // of 120 due per pass
    const int raisedAt = session.until(Action::Raise, 20, "game.exe", {"game.exe"}, drops, 10);
    require(raisedAt > 0 && reasonIs(session.last, "steady drops"), "steady drops raise");
    // The same loss at realtime: the GPU was not the cause.
    const int loweredAt = session.until(Action::Lower, 30, "game.exe", {"game.exe"}, drops, 10);
    require(loweredAt == GpuRaisePolicy::kEffectPasses && reasonIs(session.last, "raise ineffective"),
            "a raise that leaves the loss unchanged is undone after the effect window");
    require(session.until(Action::Raise, 30, "game.exe", {"game.exe"}, drops, 10) == 0,
            "an ineffective game session is not raised again");
    require(session.until(Action::Lower, 2, "", {}, 0) == 0, "game exits while lowered");
    require(session.until(Action::Raise, 20, "game.exe", {"game.exe"}, drops, 10) > 0,
            "a new session of the game may be raised again");
}

void effectiveRaiseStays() {
    Session session;
    require(session.until(Action::Raise, 20, "game.exe", {"game.exe"}, 6, 10) > 0, "steady drops raise");
    // Realtime cut the loss by more than half; occasional drops remain.
    require(session.until(Action::Lower, 60, "game.exe", {"game.exe"}, 2, 30) == 0,
            "a raise that more than halves the loss stays");
}

void partialHelpStillCountsAsIneffectiveOnlyWhenLossRemainsHigh() {
    Session session;
    require(session.until(Action::Raise, 20, "game.exe", {"game.exe"}, 12, 10) > 0, "steady drops raise");
    // 10 % at normal to 4 % at realtime: more than halved, keep.
    require(session.until(Action::Lower, 40, "game.exe", {"game.exe"}, 5, 30) == 0,
            "a raise that more than halves a heavy loss stays even above the lossy share");
}

void heldRaiseIsNotJudged() {
    Session session;
    require(session.until(Action::Raise, 20, "game.exe", {"game.exe"}, 6, 10) > 0, "steady drops raise");
    session.realtime = false;  // the gates hold it at normal
    GpuRaisePolicy::Decision decision;
    for (int index = 0; index < 40; ++index) {
        decision = session.pass("game.exe", {"game.exe"}, 6, 10);
        session.realtime = false;
        require(decision.action == Action::None, "a raise held at normal is neither judged nor quieted");
    }
}

void quietStepDownOncePerSession() {
    Session session;
    require(session.until(Action::Raise, 4, "game.exe", {"game.exe"}, 5, 60) > 0, "queued drops raise");
    // 60 s with few packets in flight and no drops.
    require(session.until(Action::Lower, 40, "game.exe", {"game.exe"}, 0, 5) > 0 &&
                reasonIs(session.last, "gpu quiet"),
            "a quiet GPU steps down");
    session.memory.insert("game.exe");
    require(session.until(Action::Raise, 5, "game.exe", {"game.exe"}, 0, 5) == 0,
            "a session lowered for quiet is not raised from memory");
}

void desktopDropsNeverRaise() {
    Session session;
    require(session.until(Action::Raise, 30, "", {"game.exe"}, 30, 90) == 0, "drops at the desktop never raise");
}

void cleanGameReadsNoQueue() {
    Session session;
    require(session.until(Action::Raise, 30, "game.exe", {"game.exe"}, 0, 90) == 0, "no drops never raise");
    require(session.reads == 0, "a game that records cleanly costs no queue reads");
    session.pass("game.exe", {"game.exe"}, 2, 90);
    require(session.reads == 1, "a pass with drops reads the queue");
}

void startRaisedHoldsUntilReset() {
    Session session;
    session.policy.startRaised();
    session.realtime = true;
    require(session.until(Action::Lower, 20, "", {}, 0) == 0, "start raised is held without a game");
    session.policy.reset();
    require(!session.policy.raised(), "reset lowers");
}
}  // namespace

int main() {
    rememberedGameInBackgroundDoesNotRaise();
    raisedGameLeavingTheFrontStepsDownAndComesBack();
    armedSessionWithoutMemoryRaisesOnReturn();
    ineffectiveRaiseIsUndone();
    effectiveRaiseStays();
    partialHelpStillCountsAsIneffectiveOnlyWhenLossRemainsHigh();
    heldRaiseIsNotJudged();
    quietStepDownOncePerSession();
    desktopDropsNeverRaise();
    cleanGameReadsNoQueue();
    startRaisedHoldsUntilReset();
    std::cout << "GPU raise policy tests passed.\n";
}
