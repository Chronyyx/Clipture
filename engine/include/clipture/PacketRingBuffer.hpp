#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace clipture {

enum class PacketKind {
    Video,
    Audio
};

using PacketPayload = std::vector<std::byte>;
using PacketPayloadPtr = std::shared_ptr<PacketPayload>;

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
    PacketPayloadPtr payload;
};

inline bool payloadEmpty(const EncodedPacket& packet) {
    return !packet.payload || packet.payload->empty();
}

inline std::size_t payloadSize(const EncodedPacket& packet) {
    return packet.payload ? packet.payload->size() : 0;
}

inline std::span<const std::byte> payloadBytes(const EncodedPacket& packet) {
    if (!packet.payload || packet.payload->empty()) return {};
    return { packet.payload->data(), packet.payload->size() };
}

inline PacketPayload& mutablePayload(EncodedPacket& packet) {
    return *packet.payload;
}

class PacketRingBuffer {
private:
    struct PayloadPool {
        std::unique_ptr<PacketPayload> acquire(std::size_t size) {
            std::unique_ptr<PacketPayload> payload;
            {
                std::lock_guard lock(mutex_);
                auto best = recycledPayloads_.end();
                for (auto it = recycledPayloads_.begin(); it != recycledPayloads_.end(); ++it) {
                    if ((*it)->capacity() < size) continue;
                    if (best == recycledPayloads_.end() || (*it)->capacity() < (*best)->capacity()) {
                        best = it;
                    }
                }
                if (best != recycledPayloads_.end()) {
                    payload = std::move(*best);
                    pooledBytes_ -= payload->capacity();
                    recycledPayloads_.erase(best);
                }
            }

            if (!payload) payload = std::make_unique<PacketPayload>();
            payload->resize(size);
            return payload;
        }

        void recycle(PacketPayload* rawPayload) {
            std::unique_ptr<PacketPayload> payload(rawPayload);
            if (!payload) return;

            const auto capacity = payload->capacity();
            if (capacity == 0 || capacity > maxSingleRecycledPayloadBytes_) return;

            std::lock_guard lock(mutex_);
            while (!recycledPayloads_.empty() && pooledBytes_ + capacity > maxRecycledPayloadBytes_) {
                pooledBytes_ -= recycledPayloads_.front()->capacity();
                recycledPayloads_.pop_front();
            }
            if (pooledBytes_ + capacity > maxRecycledPayloadBytes_) return;

            payload->clear();
            pooledBytes_ += capacity;
            recycledPayloads_.push_back(std::move(payload));
        }

        std::deque<std::unique_ptr<PacketPayload>> recycledPayloads_;
        std::size_t pooledBytes_ = 0;
        std::mutex mutex_;
        static constexpr std::size_t maxRecycledPayloadBytes_ = 64u * 1024u * 1024u;
        static constexpr std::size_t maxSingleRecycledPayloadBytes_ = 4u * 1024u * 1024u;
    };

public:
    explicit PacketRingBuffer(int64_t retention100ns = 5LL * 60LL * 10'000'000LL)
        : retention100ns_(retention100ns),
          payloadPool_(std::make_shared<PayloadPool>()) {}

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

    PacketPayloadPtr acquirePayload(std::size_t size) {
        auto payload = payloadPool_->acquire(size);
        std::weak_ptr<PayloadPool> weakPool = payloadPool_;
        return PacketPayloadPtr(payload.release(), [weakPool](PacketPayload* rawPayload) {
            if (auto pool = weakPool.lock()) {
                pool->recycle(rawPayload);
            } else {
                delete rawPayload;
            }
        });
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
            packets_.pop_front();
        }
    }

    int64_t retention100ns_;
    std::deque<EncodedPacket> packets_;
    std::shared_ptr<PayloadPool> payloadPool_;
    mutable std::mutex mutex_;
};

}  // namespace clipture
