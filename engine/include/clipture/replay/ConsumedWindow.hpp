#pragma once
#include "clipture/PacketRingBuffer.hpp"

namespace clipture::replay {
// Presentation boundary only: decoder preroll may be retained but never shown
// again. Failed saves do not call commit. Disabling restores legacy overlap.
class ConsumedWindow {
public:
    void enable(bool enabled) { if (!enabled) end_ = 0; enabled_ = enabled; }
    int64_t start() const { return enabled_ ? end_ : 0; }
    void commit(int64_t end) { if (enabled_) end_ = std::max(end_, end); }
private:
    bool enabled_ = true;
    int64_t end_ = 0;
};
std::vector<EncodedPacket> selectReplayVideo(std::vector<EncodedPacket> video,
    int durationSeconds, int64_t horizon, int64_t consumedEnd = 0);
}
