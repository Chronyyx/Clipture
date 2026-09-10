#pragma once
#include <cstdint>

namespace clipture {
// An empty WASAPI poll means no packet is ready, not that the elapsed interval
// is silent. This policy gates only fabricated silence, never captured samples.
inline bool loopbackSilenceDue(int64_t nextPts100ns, int64_t wallNow100ns,
                               int64_t packetDuration100ns) {
    // Both loopback clients request a 100 ms WASAPI buffer. Leave two such
    // periods for packet delivery before filling an actually idle interval.
    // This adds no sample queue and does not delay real captured packets.
    constexpr int64_t deliveryGrace100ns = 2'000'000;
    if (packetDuration100ns <= 0 || wallNow100ns < nextPts100ns) return false;
    const auto behind = wallNow100ns - nextPts100ns;
    return behind >= deliveryGrace100ns &&
        packetDuration100ns <= behind - deliveryGrace100ns;
}
}
