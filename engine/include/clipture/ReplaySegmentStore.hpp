#pragma once

#include "clipture/PacketRingBuffer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace clipture {

struct ReplaySegmentStoreOptions {
    std::string streamName;
    std::filesystem::path rootDirectory;
    int64_t retention100ns = 5LL * 60LL * 10'000'000LL;
    int64_t targetSegmentDuration100ns = 8LL * 10'000'000LL;
    std::size_t targetSegmentBytes = 64u * 1024u * 1024u;
    std::size_t maximumWriteBytes = 512u * 1024u;
    bool alignSegmentsToKeyframes = false;
    bool deleteOnClose = true;
};

struct ReplaySegmentStoreStats {
    bool running = false;
    bool healthy = false;
    std::size_t packets = 0;
    std::size_t queuedPackets = 0;
    std::size_t activeSegments = 0;
    uint64_t diskBytes = 0;
    uint64_t queuedBytes = 0;
    uint64_t ramFallbackBytes = 0;
    uint64_t persistedPackets = 0;
    uint64_t writeFailures = 0;
    std::size_t maximumWriteBytes = 0;
};

class ReplaySegmentStore {
public:
    explicit ReplaySegmentStore(ReplaySegmentStoreOptions options);
    ~ReplaySegmentStore();

    ReplaySegmentStore(const ReplaySegmentStore&) = delete;
    ReplaySegmentStore& operator=(const ReplaySegmentStore&) = delete;

    void start();
    void stop();
    void setRetention(int64_t retention100ns);
    void push(const EncodedPacket& packet);
    std::vector<EncodedPacket> selectWindow(int64_t startPts100ns, int64_t endPts100ns) const;
    std::vector<EncodedPacket> snapshot() const;
    void clear();
    std::size_t size() const;
    ReplaySegmentStoreStats stats() const;
    bool waitUntilIdle(std::chrono::milliseconds timeout) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipture
