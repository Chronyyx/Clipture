#pragma once

#include "clipture/PacketRingBuffer.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace clipture {

class ReplaySegmentStore;

struct AudioReplayStats {
    std::size_t queuedPackets = 0;
    std::size_t queueHighWatermark = 0;
    uint64_t queueOverflows = 0;
    uint64_t encoderRestarts = 0;
    int64_t committedPts100ns = 0;
    bool pcmRecoveryActive = false;
};

class AudioReplayCoordinator {
public:
    AudioReplayCoordinator(
        PacketRingBuffer& rawPcmPackets,
        PacketRingBuffer& aacPackets,
        ReplaySegmentStore* replayStore = nullptr,
        ReplaySegmentStore* pcmRecoveryStore = nullptr);
    ~AudioReplayCoordinator();

    AudioReplayCoordinator(const AudioReplayCoordinator&) = delete;
    AudioReplayCoordinator& operator=(const AudioReplayCoordinator&) = delete;

    void start();
    void stop();
    void publish(EncodedPacket packet);
    void updateRouting(std::map<std::string, std::string> sourceToLogicalTrack);
    void setRetention(int64_t retention100ns);
    std::vector<EncodedPacket> selectPcmWindow(int64_t startPts100ns, int64_t endPts100ns) const;
    bool waitUntil(int64_t pts100ns, std::chrono::milliseconds timeout);
    AudioReplayStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipture
