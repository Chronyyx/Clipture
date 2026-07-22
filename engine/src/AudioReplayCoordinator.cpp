#include "clipture/AudioReplayCoordinator.hpp"

#include "clipture/AacEncoderSession.hpp"
#include "clipture/AudioPacketRouting.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace clipture {
namespace {

constexpr int64_t kAlignmentWindow100ns = 2'000'000LL;
constexpr int64_t kPcmRepairWindow100ns = 20'000'000LL;
constexpr std::size_t kCoordinatorQueueCapacity = 32'768;

int64_t packetEnd(const EncodedPacket& packet) {
    return packet.pts100ns + std::max<int64_t>(1, packet.duration100ns);
}

}  // namespace

struct AudioReplayCoordinator::Impl {
    struct TrackState {
        std::string id;
        int sampleRate = 0;
        int channels = 0;
        int64_t newestEnd100ns = 0;
        int64_t nextBlockPts100ns = 0;
        int64_t committedPts100ns = 0;
        int silentBlocks = 0;
        uint32_t epoch = 1;
        std::vector<EncodedPacket> pending;
        std::vector<float> mixScratch;
        std::vector<std::byte> batch;
        uint32_t batchFrames = 0;
        int64_t batchPts100ns = 0;
        std::unique_ptr<AacEncoderSession> encoder;
    };

    PacketRingBuffer& rawPackets;
    PacketRingBuffer& aacPackets;
    std::atomic<std::shared_ptr<const std::map<std::string, std::string>>> routing;
    std::atomic<bool> running { false };
    std::thread worker;
    mutable std::mutex queueMutex;
    std::condition_variable queueCv;
    std::condition_variable progressCv;
    std::deque<EncodedPacket> queue;
    std::map<std::string, TrackState> tracks;
    std::size_t queueHighWatermark = 0;
    std::atomic<uint64_t> queueOverflows { 0 };
    std::atomic<uint64_t> encoderRestarts { 0 };
    std::atomic<int64_t> repairFromPts100ns { 0 };
    std::atomic<int64_t> latestPublishedPts100ns { 0 };
    std::atomic<int64_t> committedPts100ns { 0 };
    std::chrono::steady_clock::time_point nextRepairAttempt {};
    std::chrono::steady_clock::time_point lastPrune {};
    std::chrono::steady_clock::time_point lastRepairLog {};
    uint64_t suppressedRepairLogs = 0;

    Impl(PacketRingBuffer& raw, PacketRingBuffer& aac)
        : rawPackets(raw), aacPackets(aac) {
        routing.store(std::make_shared<const std::map<std::string, std::string>>());
    }

    void requestRepair(int64_t pts100ns) {
        int64_t current = repairFromPts100ns.load();
        while ((current == 0 || pts100ns < current) &&
               !repairFromPts100ns.compare_exchange_weak(current, pts100ns)) {
        }
        queueCv.notify_one();
    }

    void emitFrames(TrackState& track, std::vector<AacEncodedFrame>& frames) {
        for (auto& frame : frames) {
            if (frame.payload.empty()) continue;
            EncodedPacket packet;
            packet.kind = PacketKind::Audio;
            packet.codec = PacketCodec::AacLc;
            packet.pts100ns = frame.pts100ns;
            packet.dts100ns = frame.pts100ns;
            packet.duration100ns =
                (static_cast<int64_t>(frame.durationFrames) * 10'000'000LL) / std::max(1, track.sampleRate);
            packet.sourceId = track.id;
            packet.logicalTrackId = track.id;
            packet.encoderId = "AAC_MF_LIVE";
            packet.sampleRate = track.sampleRate;
            packet.channelCount = track.channels;
            packet.bitsPerSample = 0;
            packet.audioFrameCount = frame.durationFrames;
            packet.audioPrimingFrames = frame.primingFrames;
            packet.encoderEpoch = track.epoch;
            packet.audible = true;
            packet.payload = aacPackets.acquirePayload(frame.payload.size());
            std::memcpy(mutablePayload(packet).data(), frame.payload.data(), frame.payload.size());
            aacPackets.push(std::move(packet));
        }
        frames.clear();
    }

    bool ensureEncoder(TrackState& track, std::string& error) {
        if (track.encoder && track.encoder->active()) return true;
        track.encoder = std::make_unique<AacEncoderSession>();
        if (!track.encoder->start(track.sampleRate, track.channels, error)) {
            track.encoder.reset();
            return false;
        }
        return true;
    }

    bool flushBatch(TrackState& track) {
        if (track.batchFrames == 0) return true;
        std::string error;
        if (!ensureEncoder(track, error)) {
            std::cerr << "[audio-replay] AAC encoder start failed track=\"" << track.id
                      << "\" error=\"" << error << "\"" << std::endl;
            requestRepair(track.batchPts100ns);
            nextRepairAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            return false;
        }

        std::vector<AacEncodedFrame> frames;
        if (!track.encoder->encode(track.batch, track.batchPts100ns, track.batchFrames, frames, error)) {
            std::cerr << "[audio-replay] AAC encode failed track=\"" << track.id
                      << "\" error=\"" << error << "\"" << std::endl;
            track.encoder.reset();
            requestRepair(track.batchPts100ns);
            nextRepairAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            return false;
        }
        emitFrames(track, frames);
        track.batch.clear();
        track.batchFrames = 0;
        return true;
    }

    bool finishEpoch(TrackState& track) {
        if (!flushBatch(track)) return false;
        if (track.encoder && track.encoder->active()) {
            std::string error;
            std::vector<AacEncodedFrame> frames;
            if (!track.encoder->finish(frames, error)) {
                std::cerr << "[audio-replay] AAC drain failed track=\"" << track.id
                          << "\" error=\"" << error << "\"" << std::endl;
                requestRepair(track.committedPts100ns);
                track.encoder.reset();
                return false;
            }
            emitFrames(track, frames);
        }
        track.encoder.reset();
        ++track.epoch;
        track.silentBlocks = 0;
        return true;
    }

    bool processOneBlock(TrackState& track, bool force) {
        if (track.pending.empty() || track.sampleRate <= 0 || track.channels <= 0) return false;
        const int framesPerBlock = std::max(1, track.sampleRate / 100);
        const int64_t blockDuration100ns =
            (static_cast<int64_t>(framesPerBlock) * 10'000'000LL) / track.sampleRate;
        if (track.nextBlockPts100ns == 0) {
            track.nextBlockPts100ns = track.pending.front().pts100ns;
        }
        if (!force && track.newestEnd100ns < track.nextBlockPts100ns + blockDuration100ns + kAlignmentWindow100ns) {
            return false;
        }

        const int64_t blockStart = track.nextBlockPts100ns;
        const int64_t blockEnd = blockStart + blockDuration100ns;
        track.pending.erase(
            track.pending.begin(),
            std::find_if(track.pending.begin(), track.pending.end(), [&](const EncodedPacket& packet) {
                return packetEnd(packet) > blockStart;
            }));

        const std::size_t sampleCount = static_cast<std::size_t>(framesPerBlock) * track.channels;
        track.mixScratch.assign(sampleCount, 0.0f);
        for (const auto& packet : track.pending) {
            if (packet.pts100ns >= blockEnd) break;
            if (packet.sampleRate != track.sampleRate || packet.channelCount != track.channels) continue;
            const auto bytes = payloadBytes(packet);
            const auto* input = reinterpret_cast<const int16_t*>(bytes.data());
            const int64_t inputFrames = static_cast<int64_t>(bytes.size() / (sizeof(int16_t) * track.channels));
            const int64_t relativeStart =
                ((packet.pts100ns - blockStart) * track.sampleRate) / 10'000'000LL;
            const int64_t outputStart = std::max<int64_t>(0, relativeStart);
            const int64_t inputStart = std::max<int64_t>(0, -relativeStart);
            const int64_t copiedFrames = std::min<int64_t>(
                framesPerBlock - outputStart,
                inputFrames - inputStart);
            if (copiedFrames <= 0) continue;
            for (int64_t frame = 0; frame < copiedFrames; ++frame) {
                for (int channel = 0; channel < track.channels; ++channel) {
                    const auto outputIndex = static_cast<std::size_t>((outputStart + frame) * track.channels + channel);
                    const auto inputIndex = static_cast<std::size_t>((inputStart + frame) * track.channels + channel);
                    track.mixScratch[outputIndex] += input[inputIndex];
                }
            }
        }

        float peak = 0.0f;
        for (const float sample : track.mixScratch) peak = std::max(peak, std::abs(sample));
        const bool audible = peak > 0.0f;
        const bool epochActive = audible || track.batchFrames > 0 || (track.encoder && track.encoder->active());
        if (epochActive) {
            if (track.batchFrames == 0) track.batchPts100ns = blockStart;
            const float limiterGain = peak > 32767.0f ? 32767.0f / peak : 1.0f;
            const std::size_t oldSize = track.batch.size();
            track.batch.resize(oldSize + sampleCount * sizeof(int16_t));
            auto* destination = reinterpret_cast<int16_t*>(track.batch.data() + oldSize);
            for (std::size_t i = 0; i < sampleCount; ++i) {
                destination[i] = static_cast<int16_t>(std::clamp(
                    track.mixScratch[i] * limiterGain,
                    -32768.0f,
                    32767.0f));
            }
            track.batchFrames += static_cast<uint32_t>(framesPerBlock);
            const uint32_t targetBatchFrames = static_cast<uint32_t>(std::max(1, track.sampleRate / 10));
            if (track.batchFrames >= targetBatchFrames && !flushBatch(track)) return false;
        }

        track.silentBlocks = audible ? 0 : track.silentBlocks + 1;
        track.nextBlockPts100ns = blockEnd;
        track.committedPts100ns = blockEnd;
        if (track.silentBlocks >= 20 && track.encoder && !finishEpoch(track)) return false;
        return true;
    }

    void updateCommittedWatermark() {
        int64_t minimum = std::numeric_limits<int64_t>::max();
        for (const auto& [_, track] : tracks) {
            if (track.committedPts100ns > 0) minimum = std::min(minimum, track.committedPts100ns);
        }
        committedPts100ns.store(minimum == std::numeric_limits<int64_t>::max() ? 0 : minimum);
        progressCv.notify_all();
    }

    void processPackets(std::vector<EncodedPacket>& incoming, bool force) {
        std::sort(incoming.begin(), incoming.end(), [](const EncodedPacket& left, const EncodedPacket& right) {
            return left.pts100ns < right.pts100ns;
        });
        for (auto& packet : incoming) {
            if (packet.codec != PacketCodec::PcmS16 || packet.logicalTrackId.empty()) continue;
            auto& track = tracks[packet.logicalTrackId];
            track.id = packet.logicalTrackId;
            if (track.sampleRate == 0) {
                track.sampleRate = packet.sampleRate;
                track.channels = packet.channelCount;
            }
            if (packet.sampleRate != track.sampleRate || packet.channelCount != track.channels) continue;
            track.newestEnd100ns = std::max(track.newestEnd100ns, packetEnd(packet));
            track.pending.push_back(std::move(packet));
        }
        incoming.clear();

        for (auto& [_, track] : tracks) {
            std::sort(track.pending.begin(), track.pending.end(), [](const EncodedPacket& left, const EncodedPacket& right) {
                return left.pts100ns < right.pts100ns;
            });
        }
        bool progressed = true;
        while (progressed) {
            progressed = false;
            for (auto& [_, track] : tracks) {
                progressed = processOneBlock(track, force) || progressed;
            }
        }
        updateCommittedWatermark();
    }

    void rebuildIfNeeded(std::vector<EncodedPacket>& incoming) {
        int64_t repairFrom = 0;
        std::deque<EncodedPacket> discardedQueue;
        if (repairFromPts100ns.load() != 0 && std::chrono::steady_clock::now() >= nextRepairAttempt) {
            repairFrom = repairFromPts100ns.exchange(0);
            repairFrom = std::max<int64_t>(0, repairFrom - kPcmRepairWindow100ns);
            std::lock_guard lock(queueMutex);
            discardedQueue.swap(queue);
        }
        if (repairFrom == 0) return;

        for (auto& [_, track] : tracks) {
            if (track.encoder) track.encoder->reset();
        }
        tracks.clear();
        aacPackets.eraseIf([&](const EncodedPacket& packet) {
            return packet.codec == PacketCodec::AacLc && packet.pts100ns >= repairFrom;
        });
        incoming = rawPackets.selectWindow(repairFrom, std::numeric_limits<int64_t>::max());
        int64_t snapshotEnd100ns = repairFrom;
        for (const auto& packet : incoming) snapshotEnd100ns = std::max(snapshotEnd100ns, packetEnd(packet));
        std::deque<EncodedPacket> queuedDuringSnapshot;
        {
            std::lock_guard lock(queueMutex);
            queuedDuringSnapshot.swap(queue);
        }
        std::erase_if(queuedDuringSnapshot, [&](const EncodedPacket& packet) {
            return packetEnd(packet) <= snapshotEnd100ns;
        });
        {
            std::lock_guard lock(queueMutex);
            queuedDuringSnapshot.insert(
                queuedDuringSnapshot.end(),
                std::make_move_iterator(queue.begin()),
                std::make_move_iterator(queue.end()));
            queue.swap(queuedDuringSnapshot);
        }
        ++encoderRestarts;
        const auto now = std::chrono::steady_clock::now();
        if (lastRepairLog.time_since_epoch().count() == 0 || now - lastRepairLog >= std::chrono::seconds(10)) {
            std::cerr << "[audio-replay] repairing AAC coverage fromPts100ns=" << repairFrom
                      << " pcmPackets=" << incoming.size()
                      << " suppressed=" << suppressedRepairLogs << std::endl;
            lastRepairLog = now;
            suppressedRepairLogs = 0;
        } else {
            ++suppressedRepairLogs;
        }
    }

    void pruneRawPcm() {
        const auto now = std::chrono::steady_clock::now();
        if (lastPrune.time_since_epoch().count() != 0 && now - lastPrune < std::chrono::seconds(1)) return;
        lastPrune = now;

        std::map<std::string, int64_t> committedByTrack;
        for (const auto& [id, track] : tracks) committedByTrack[id] = track.committedPts100ns;
        int64_t frozenFrom = 0;
        int64_t latestPublished = 0;
        {
            std::lock_guard lock(queueMutex);
            frozenFrom = repairFromPts100ns.load();
            latestPublished = latestPublishedPts100ns.load();
        }
        rawPackets.eraseIf([&](const EncodedPacket& packet) {
            if (packet.codec != PacketCodec::PcmS16) return false;
            if (packet.logicalTrackId.empty()) {
                return packetEnd(packet) < latestPublished - kPcmRepairWindow100ns;
            }
            const auto found = committedByTrack.find(packet.logicalTrackId);
            if (found == committedByTrack.end() || found->second == 0) return false;
            int64_t trimBefore = found->second - kPcmRepairWindow100ns;
            if (frozenFrom > 0) trimBefore = std::min(trimBefore, frozenFrom);
            return packetEnd(packet) < trimBefore;
        });
    }

    void run() {
        const int previousPriority = GetThreadPriority(GetCurrentThread());
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::vector<EncodedPacket> incoming;
        while (running.load()) {
            std::deque<EncodedPacket> drained;
            {
                std::unique_lock lock(queueMutex);
                queueCv.wait_for(lock, std::chrono::milliseconds(10), [&] {
                    return !running.load() || !queue.empty() || repairFromPts100ns.load() != 0;
                });
                drained.swap(queue);
            }
            incoming.reserve(incoming.size() + drained.size());
            while (!drained.empty()) {
                incoming.push_back(std::move(drained.front()));
                drained.pop_front();
            }
            rebuildIfNeeded(incoming);
            processPackets(incoming, false);
            pruneRawPcm();
        }

        std::deque<EncodedPacket> drained;
        {
            std::lock_guard lock(queueMutex);
            drained.swap(queue);
        }
        incoming.reserve(incoming.size() + drained.size());
        while (!drained.empty()) {
            incoming.push_back(std::move(drained.front()));
            drained.pop_front();
        }
        processPackets(incoming, true);
        for (auto& [_, track] : tracks) finishEpoch(track);
        updateCommittedWatermark();
        if (previousPriority != THREAD_PRIORITY_ERROR_RETURN) {
            SetThreadPriority(GetCurrentThread(), previousPriority);
        }
    }
};

AudioReplayCoordinator::AudioReplayCoordinator(PacketRingBuffer& rawPcmPackets, PacketRingBuffer& aacPackets)
    : impl_(std::make_unique<Impl>(rawPcmPackets, aacPackets)) {}

AudioReplayCoordinator::~AudioReplayCoordinator() {
    stop();
}

void AudioReplayCoordinator::start() {
    if (impl_->running.exchange(true)) return;
    impl_->worker = std::thread([this] { impl_->run(); });
}

void AudioReplayCoordinator::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->queueCv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void AudioReplayCoordinator::publish(EncodedPacket packet) {
    const auto routes = impl_->routing.load();
    static const std::map<std::string, std::string> emptyRoutes;
    prepareAudioReplayPacket(packet, routes ? *routes : emptyRoutes);

    impl_->rawPackets.push(packet);
    int64_t latest = impl_->latestPublishedPts100ns.load();
    const int64_t endPts100ns = packetEnd(packet);
    while (endPts100ns > latest &&
           !impl_->latestPublishedPts100ns.compare_exchange_weak(latest, endPts100ns)) {
    }
    if (!impl_->running.load() || packet.logicalTrackId.empty()) return;
    {
        std::lock_guard lock(impl_->queueMutex);
        if (impl_->queue.size() >= kCoordinatorQueueCapacity) {
            ++impl_->queueOverflows;
            impl_->requestRepair(packet.pts100ns);
            return;
        }
        impl_->queue.push_back(std::move(packet));
        impl_->queueHighWatermark = std::max(impl_->queueHighWatermark, impl_->queue.size());
    }
    impl_->queueCv.notify_one();
}

void AudioReplayCoordinator::updateRouting(std::map<std::string, std::string> sourceToLogicalTrack) {
    impl_->routing.store(
        std::make_shared<const std::map<std::string, std::string>>(std::move(sourceToLogicalTrack)));
}

void AudioReplayCoordinator::setRetention(int64_t retention100ns) {
    impl_->aacPackets.setRetention(retention100ns);
    impl_->rawPackets.setRetention(retention100ns);
}

bool AudioReplayCoordinator::waitUntil(int64_t pts100ns, std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->queueMutex);
    return impl_->progressCv.wait_for(lock, timeout, [&] {
        return impl_->committedPts100ns.load() >= pts100ns || !impl_->running.load();
    });
}

AudioReplayStats AudioReplayCoordinator::stats() const {
    std::lock_guard lock(impl_->queueMutex);
    return {
        impl_->queue.size(),
        impl_->queueHighWatermark,
        impl_->queueOverflows.load(),
        impl_->encoderRestarts.load(),
        impl_->committedPts100ns.load()
    };
}

}  // namespace clipture
