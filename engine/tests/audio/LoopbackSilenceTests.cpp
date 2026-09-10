#include "clipture/LoopbackSilencePolicy.hpp"
#include "clipture/AudioTimeline.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
constexpr int64_t ms = 10'000;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void continuousPacketsWithEmptyPolls() {
    // Twenty ms packets arrive ten ms after their end. Ten ms polling sees an
    // empty buffer between packets. The former fallback inserted zeros there,
    // then the forward-only clock shifted real audio after those zeros.
    int64_t next = 10'000 * ms;
    int fabricated = 0;
    for (int elapsed = 10; elapsed <= 4000; elapsed += 10) {
        if (elapsed >= 30 && elapsed % 20 == 10) {
            next += 20 * ms;
        } else {
            for (int fills = 0; fills < 5 && clipture::loopbackSilenceDue(
                     next, (10'000 + elapsed) * ms, 10 * ms); ++fills) {
                next += 10 * ms;
                ++fabricated;
            }
        }
    }
    require(fabricated == 0, "empty polls injected silence into continuous loopback audio");
}

void genuinelyIdleSourceStillAdvances() {
    constexpr int64_t now = 5000 * ms;
    int64_t next = 4000 * ms;
    int fills = 0;
    while (clipture::loopbackSilenceDue(next, now, 10 * ms)) {
        next += 10 * ms;
        require(++fills <= 100, "silence fallback did not stop");
    }
    require(fills > 0, "idle sources must still publish silence for audio watermarks");
    require(next == now - 200 * ms, "idle silence must stop at the delivery grace boundary");
}

void continuousPacketsWithDeliveryJitter() {
    // Exercise the actual forward-only timestamp aligner too: no packet may
    // be shifted after made-up silence, including when the driver batches
    // several packets in one wakeup. Captured samples are never held back.
    for (const int packetMs : {5, 10, 20, 40, 100}) {
        constexpr std::array<int, 6> deliveryDelays {0, 10, 30, 80, 10, 0};
        constexpr int64_t origin = 10'000 * ms;
        int64_t next = origin;
        bool anchored = false;
        int packetIndex = 0;
        for (int elapsed = 0; elapsed <= 4000; elapsed += 5) {
            bool captured = false;
            while ((packetIndex + 1) * packetMs +
                   deliveryDelays[packetIndex % deliveryDelays.size()] <= elapsed) {
                const int64_t pts = origin + packetIndex * packetMs * ms;
                clipture::alignAudioPtsForwardOnly(pts, packetMs * ms, anchored, next);
                require(next == pts, "continuous packet shifted by speculative silence");
                next += packetMs * ms;
                ++packetIndex;
                captured = true;
            }
            if (!captured) {
                require(!clipture::loopbackSilenceDue(next, origin + elapsed * ms, 10 * ms),
                        "batched packet delivery mistaken for source silence");
            }
        }
        require(packetIndex > 0, "jitter fixture did not deliver audio");
    }
}

void idleSourceResumesAtCaptureClock() {
    int64_t next = 4000 * ms;
    constexpr int64_t now = 5000 * ms;
    while (clipture::loopbackSilenceDue(next, now, 10 * ms)) next += 10 * ms;
    bool anchored = true;
    clipture::alignAudioPtsForwardOnly(now, 10 * ms, anchored, next);
    require(next == now, "resumed audio was shifted away from its capture timestamp");
    require(!clipture::loopbackSilenceDue(now, now, 10 * ms), "empty poll at head is not silence");
    require(!clipture::loopbackSilenceDue(now, now - ms, 10 * ms), "clock regression fabricated silence");
    require(!clipture::loopbackSilenceDue(now, now + 1000 * ms, 0), "zero duration would spin forever");
    require(!clipture::loopbackSilenceDue(now, now + 209 * ms, 10 * ms), "partial grace crossing emitted silence");
    require(clipture::loopbackSilenceDue(now, now + 210 * ms, 10 * ms), "fully aged silence did not advance");
}
}

int main() {
    try {
        continuousPacketsWithEmptyPolls();
        continuousPacketsWithDeliveryJitter();
        genuinelyIdleSourceStillAdvances();
        idleSourceResumesAtCaptureClock();
        std::cout << "Loopback silence policy passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
