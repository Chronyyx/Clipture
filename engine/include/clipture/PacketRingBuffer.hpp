#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace clipture {

enum class PacketKind {
    Video,
    Audio
};

struct EncodedPacket {
    PacketKind kind = PacketKind::Video;
    int64_t pts100ns = 0;
    int64_t dts100ns = 0;
    int64_t duration100ns = 0;
    bool keyframe = false;
    std::string sourceId;
    std::string encoderId;
    int sampleRate = 0;
    int channelCount = 0;
    int bitsPerSample = 0;
    int encodedWidth = 0;
    int encodedHeight = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    std::vector<std::byte> payload;
};

class PacketRingBuffer {
public:
    explicit PacketRingBuffer(int64_t retention100ns = 5LL * 60LL * 10'000'000LL)
        : retention100ns_(retention100ns) {}

    void setRetention(int64_t retention100ns) {
        std::lock_guard lock(mutex_);
        retention100ns_ = retention100ns;
        trimLocked();
    }

    void push(EncodedPacket packet) {
        std::lock_guard lock(mutex_);
        packets_.push_back(std::move(packet));
        trimLocked();
    }

    std::vector<std::byte> acquirePayload(std::size_t size) {
        std::lock_guard lock(mutex_);
        auto best = recycledPayloads_.end();
        for (auto it = recycledPayloads_.begin(); it != recycledPayloads_.end(); ++it) {
            if (it->capacity() < size) continue;
            if (best == recycledPayloads_.end() || it->capacity() < best->capacity()) {
                best = it;
            }
        }
        if (best == recycledPayloads_.end()) {
            return std::vector<std::byte>(size);
        }

        std::vector<std::byte> payload = std::move(*best);
        pooledBytes_ -= payload.capacity();
        recycledPayloads_.erase(best);
        payload.resize(size);
        return payload;
    }

    std::vector<EncodedPacket> selectWindow(int64_t startPts100ns, int64_t endPts100ns) const {
        std::lock_guard lock(mutex_);
        std::vector<EncodedPacket> selected;
        for (const auto& packet : packets_) {
            if (packet.pts100ns >= startPts100ns && packet.pts100ns <= endPts100ns) {
                selected.push_back(packet);
            }
        }
        return selected;
    }

    std::vector<EncodedPacket> snapshot() const {
        std::lock_guard lock(mutex_);
        return { packets_.begin(), packets_.end() };
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return packets_.size();
    }

private:
    void trimLocked() {
        if (packets_.empty()) return;
        const auto newestPts = packets_.back().pts100ns;
        const auto oldestAllowed = newestPts - retention100ns_;
        while (!packets_.empty() && packets_.front().pts100ns < oldestAllowed) {
            recyclePayloadLocked(std::move(packets_.front().payload));
            packets_.pop_front();
        }
    }

    void recyclePayloadLocked(std::vector<std::byte> payload) {
        const auto capacity = payload.capacity();
        if (capacity == 0 || capacity > maxSingleRecycledPayloadBytes_) return;
        while (!recycledPayloads_.empty() && pooledBytes_ + capacity > maxRecycledPayloadBytes_) {
            pooledBytes_ -= recycledPayloads_.front().capacity();
            recycledPayloads_.pop_front();
        }
        if (pooledBytes_ + capacity > maxRecycledPayloadBytes_) return;
        payload.clear();
        pooledBytes_ += capacity;
        recycledPayloads_.push_back(std::move(payload));
    }

    int64_t retention100ns_;
    std::deque<EncodedPacket> packets_;
    std::deque<std::vector<std::byte>> recycledPayloads_;
    std::size_t pooledBytes_ = 0;
    mutable std::mutex mutex_;
    static constexpr std::size_t maxRecycledPayloadBytes_ = 64u * 1024u * 1024u;
    static constexpr std::size_t maxSingleRecycledPayloadBytes_ = 4u * 1024u * 1024u;
};

}  // namespace clipture
