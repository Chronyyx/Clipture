#include "clipture/ReplaySegmentStore.hpp"

#include "clipture/BoundedWrite.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace clipture {
namespace {

constexpr auto kRetryDelay = std::chrono::milliseconds(500);
constexpr auto kStaleSessionAge = std::chrono::hours(24);
constexpr std::size_t kReadAheadBytes = 2 * 1024 * 1024;

std::filesystem::path defaultReplayRoot() {
    std::wstring localAppData(32'768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        localAppData.data(),
        static_cast<DWORD>(localAppData.size()));
    if (length > 0 && length < localAppData.size()) {
        localAppData.resize(length);
        return std::filesystem::path(localAppData) / L"Clipture" / L"ReplayCache";
    }

    std::wstring temporary(32'768, L'\0');
    const DWORD temporaryLength = GetTempPathW(
        static_cast<DWORD>(temporary.size()),
        temporary.data());
    if (temporaryLength > 0 && temporaryLength < temporary.size()) {
        temporary.resize(temporaryLength);
        return std::filesystem::path(temporary) / L"CliptureReplayCache";
    }
    return std::filesystem::current_path() / L"CliptureReplayCache";
}

std::wstring safeStreamName(const std::string& streamName) {
    std::wstring result;
    result.reserve(streamName.size());
    for (const unsigned char ch : streamName) {
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
            result.push_back(static_cast<wchar_t>(ch));
        }
    }
    return result.empty() ? L"packets" : result;
}

void removeStaleSessions(const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::exists(root, error)) return;
    const auto cutoff = std::filesystem::file_time_type::clock::now() - kStaleSessionAge;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_directory(error)) continue;
        const auto name = it->path().filename().wstring();
        if (!name.starts_with(L"session-")) continue;
        const std::size_t pidStart = std::wstring(L"session-").size();
        const std::size_t pidEnd = name.find(L'-', pidStart);
        bool confirmedDead = false;
        if (pidEnd != std::wstring::npos) {
            try {
                const DWORD processId = static_cast<DWORD>(std::stoul(name.substr(pidStart, pidEnd - pidStart)));
                HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
                if (process) {
                    const DWORD waitResult = WaitForSingleObject(process, 0);
                    CloseHandle(process);
                    if (waitResult == WAIT_TIMEOUT) continue;
                    if (waitResult == WAIT_OBJECT_0) confirmedDead = true;
                } else {
                    const DWORD openError = GetLastError();
                    if (openError == ERROR_ACCESS_DENIED) continue;
                    if (openError == ERROR_INVALID_PARAMETER) confirmedDead = true;
                }
            } catch (...) {
            }
        }
        if (confirmedDead) {
            std::filesystem::remove_all(it->path(), error);
            error.clear();
            continue;
        }
        const auto modified = it->last_write_time(error);
        if (error) {
            error.clear();
            continue;
        }
        if (modified < cutoff) std::filesystem::remove_all(it->path(), error);
        error.clear();
    }
}

struct ReplaySessionDirectory {
    std::filesystem::path path;
    bool deleteOnClose = true;

    ~ReplaySessionDirectory() {
        if (!deleteOnClose || path.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

bool applyLowIoPriority(HANDLE handle);

struct SegmentBacking;

struct SegmentReadCache {
    ~SegmentReadCache() {
        std::lock_guard lock(mutex);
        closeLocked();
    }

    void closeLocked() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
        owner = nullptr;
        fileOffset = 0;
        fileOffsetKnown = false;
        readAheadOffset = 0;
        readAheadSize = 0;
    }

    uint64_t capacityBytes() const {
        std::lock_guard lock(mutex);
        return static_cast<uint64_t>(readAhead.capacity());
    }

    mutable std::mutex mutex;
    HANDLE handle = INVALID_HANDLE_VALUE;
    const SegmentBacking* owner = nullptr;
    uint64_t fileOffset = 0;
    bool fileOffsetKnown = false;
    uint64_t readAheadOffset = 0;
    std::size_t readAheadSize = 0;
    std::vector<std::byte> readAhead;
};

struct SegmentBacking {
    std::shared_ptr<ReplaySessionDirectory> session;
    std::shared_ptr<SegmentReadCache> readCache;
    std::filesystem::path path;
    std::atomic<uint64_t> writtenBytes { 0 };

    ~SegmentBacking() {
        if (readCache) {
            std::lock_guard lock(readCache->mutex);
            if (readCache->owner == this) {
                readCache->closeLocked();
            }
        }
        if (!session || !session->deleteOnClose || path.empty()) return;
        DeleteFileW(path.c_str());
    }

    bool read(uint64_t offset, std::span<std::byte> destination) const {
        if (destination.empty()) return true;
        if (offset > static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max())) return false;
        const uint64_t availableBytes = writtenBytes.load(std::memory_order_acquire);
        if (offset > availableBytes || destination.size() > availableBytes - offset) return false;
        if (!readCache) return false;

        std::lock_guard lock(readCache->mutex);
        if (readCache->owner != this || readCache->handle == INVALID_HANDLE_VALUE) {
            readCache->closeLocked();
            readCache->handle = CreateFileW(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr);
            if (readCache->handle == INVALID_HANDLE_VALUE) return false;
            applyLowIoPriority(readCache->handle);
            readCache->owner = this;
            readCache->fileOffset = 0;
            readCache->fileOffsetKnown = true;
            readCache->readAhead.reserve(kReadAheadBytes);
        }

        uint64_t currentOffset = offset;
        while (!destination.empty()) {
            if (currentOffset >= readCache->readAheadOffset &&
                currentOffset - readCache->readAheadOffset < readCache->readAheadSize) {
                const std::size_t cacheOffset =
                    static_cast<std::size_t>(currentOffset - readCache->readAheadOffset);
                const std::size_t copyBytes =
                    std::min(destination.size(), readCache->readAheadSize - cacheOffset);
                std::memcpy(destination.data(), readCache->readAhead.data() + cacheOffset, copyBytes);
                destination = destination.subspan(copyBytes);
                currentOffset += copyBytes;
                continue;
            }

            const uint64_t readableBytes = writtenBytes.load(std::memory_order_acquire);
            if (currentOffset >= readableBytes) return false;
            const std::size_t request = static_cast<std::size_t>(std::min<uint64_t>(
                kReadAheadBytes,
                readableBytes - currentOffset));
            if (request == 0) return false;

            if (!readCache->fileOffsetKnown || readCache->fileOffset != currentOffset) {
                LARGE_INTEGER position {};
                position.QuadPart = static_cast<LONGLONG>(currentOffset);
                if (!SetFilePointerEx(readCache->handle, position, nullptr, FILE_BEGIN)) {
                    readCache->fileOffsetKnown = false;
                    return false;
                }
                readCache->fileOffset = currentOffset;
                readCache->fileOffsetKnown = true;
            }

            readCache->readAhead.resize(request);
            DWORD readBytes = 0;
            if (!ReadFile(
                    readCache->handle,
                    readCache->readAhead.data(),
                    static_cast<DWORD>(request),
                    &readBytes,
                    nullptr) || readBytes != request) {
                readCache->fileOffsetKnown = false;
                readCache->readAheadSize = 0;
                return false;
            }
            readCache->fileOffset += readBytes;
            readCache->readAheadOffset = currentOffset;
            readCache->readAheadSize = readBytes;
        }
        return true;
    }
};

class SegmentPayloadReader final : public PacketPayloadReader {
public:
    SegmentPayloadReader(
        std::shared_ptr<SegmentBacking> backing,
        uint64_t offset,
        std::size_t size)
        : backing_(std::move(backing)), offset_(offset), size_(size) {}

    std::size_t size() const noexcept override { return size_; }

    bool read(std::size_t offset, std::span<std::byte> destination) const override {
        if (offset > size_ || destination.size() > size_ - offset || !backing_) return false;
        return backing_->read(offset_ + offset, destination);
    }

private:
    std::shared_ptr<SegmentBacking> backing_;
    uint64_t offset_ = 0;
    std::size_t size_ = 0;
};

bool applyLowIoPriority(HANDLE handle) {
    FILE_IO_PRIORITY_HINT_INFO priorityInfo {};
    priorityInfo.PriorityHint = IoPriorityHintLow;
    return SetFileInformationByHandle(
        handle,
        FileIoPriorityHintInfo,
        &priorityInfo,
        sizeof(priorityInfo)) != FALSE;
}

}  // namespace

struct ReplaySegmentStore::Impl {
    struct Entry {
        EncodedPacket packet;
        std::atomic<bool> retired { false };
        bool persistenceQueued = false;
        std::size_t payloadBytes = 0;
    };

    struct ActiveSegment {
        std::shared_ptr<SegmentBacking> backing;
        HANDLE handle = INVALID_HANDLE_VALUE;
        uint64_t offset = 0;
        int64_t firstPts100ns = 0;

        bool valid() const { return handle != INVALID_HANDLE_VALUE && backing; }
    };

    explicit Impl(ReplaySegmentStoreOptions requestedOptions)
        : options(std::move(requestedOptions)) {
        options.retention100ns = std::max<int64_t>(1, options.retention100ns);
        options.targetSegmentDuration100ns = std::max<int64_t>(1, options.targetSegmentDuration100ns);
        options.targetSegmentBytes = std::max<std::size_t>(1, options.targetSegmentBytes);
        options.maximumWriteBytes = std::max<std::size_t>(1, options.maximumWriteBytes);
    }

    ~Impl() { stop(); }

    void start() {
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true)) return;
        stopRequested = false;
        {
            std::lock_guard lock(segmentMutex);
            healthy = true;
        }
        worker = std::thread([this] { run(); });
    }

    void stop() {
        if (!running.exchange(false)) return;
        {
            std::lock_guard lock(mutex);
            stopRequested = true;
            for (const auto& entry : entries) entry->retired = true;
            pending.clear();
            queuedPackets = 0;
            queuedBytes = 0;
            queuedResidentBytes = 0;
            nextSpillCandidate = 0;
            newestPts100ns = std::numeric_limits<int64_t>::min();
        }
        wake.notify_all();
        idle.notify_all();
        if (worker.joinable()) worker.join();
        {
            std::lock_guard lock(mutex);
            entries.clear();
            residentPayloadBytes = 0;
            residentPackets = 0;
            diskBackedPackets = 0;
        }
        {
            std::lock_guard lock(segmentMutex);
            closeActive();
            segments.clear();
            session.reset();
            healthy = false;
        }
    }

    void setRetention(int64_t retention100ns) {
        std::lock_guard lock(mutex);
        options.retention100ns = std::max<int64_t>(1, retention100ns);
        trimLocked();
        wake.notify_one();
    }

    void setResidentPayloadBudget(std::size_t bytes) {
        std::lock_guard lock(mutex);
        options.residentPayloadBudgetBytes = bytes;
        scheduleSpillsLocked();
        wake.notify_one();
    }

    void push(const EncodedPacket& packet) {
        if (payloadEmpty(packet)) return;
        auto entry = std::make_shared<Entry>();
        entry->packet = packet;
        entry->payloadBytes = payloadSize(packet);
        const auto bytes = static_cast<uint64_t>(entry->payloadBytes);
        {
            std::lock_guard lock(mutex);
            entries.push_back(entry);
            newestPts100ns = std::max(newestPts100ns, packet.pts100ns);
            if (packet.payload) {
                residentPayloadBytes += bytes;
                ++residentPackets;
            } else if (packet.payloadReader) {
                ++diskBackedPackets;
            }
            trimLocked();
            scheduleSpillsLocked();
        }
        wake.notify_one();
    }

    std::vector<EncodedPacket> selectWindow(int64_t startPts100ns, int64_t endPts100ns) const {
        std::lock_guard lock(mutex);
        std::vector<EncodedPacket> selected;
        for (const auto& entry : entries) {
            if (entry->retired.load(std::memory_order_relaxed)) continue;
            if (entry->packet.pts100ns >= startPts100ns && entry->packet.pts100ns <= endPts100ns) {
                selected.push_back(entry->packet);
            }
        }
        return selected;
    }

    std::vector<EncodedPacket> snapshot() const {
        std::lock_guard lock(mutex);
        std::vector<EncodedPacket> result;
        result.reserve(entries.size());
        for (const auto& entry : entries) {
            if (!entry->retired.load(std::memory_order_relaxed)) result.push_back(entry->packet);
        }
        return result;
    }

    void clear() {
        std::lock_guard lock(mutex);
        for (const auto& entry : entries) entry->retired = true;
        entries.clear();
        pending.clear();
        queuedPackets = 0;
        queuedBytes = 0;
        queuedResidentBytes = 0;
        residentPayloadBytes = 0;
        residentPackets = 0;
        diskBackedPackets = 0;
        nextSpillCandidate = 0;
        newestPts100ns = std::numeric_limits<int64_t>::min();
        rotationRequested.store(true, std::memory_order_relaxed);
        wake.notify_one();
        idle.notify_all();
    }

    std::size_t size() const {
        std::lock_guard lock(mutex);
        return entries.size();
    }

    ReplaySegmentStoreStats stats() const {
        ReplaySegmentStoreStats result;
        result.running = running.load();
        {
            std::lock_guard lock(mutex);
            result.packets = entries.size();
            result.queuedPackets = queuedPackets;
            result.queuedBytes = queuedBytes;
            result.residentPayloadBytes = residentPayloadBytes;
            result.residentPayloadBudgetBytes = options.residentPayloadBudgetBytes;
            result.persistedPackets = persistedPackets;
            result.residentPackets = residentPackets;
            result.diskBackedPackets = diskBackedPackets;
            result.spillCandidateInspections = spillCandidateInspections;
        }
        {
            std::lock_guard lock(segmentMutex);
            result.healthy = healthy;
            result.activeSegments = segments.size();
            result.writeFailures = writeFailures;
            result.maximumWriteBytes = maximumWriteSize;
            for (const auto& segment : segments) {
                result.diskBytes += segment->writtenBytes.load(std::memory_order_relaxed);
            }
        }
        result.readCacheBytes = readCache ? readCache->capacityBytes() : 0;
        result.ramFallbackBytes = result.healthy ? 0 : result.queuedBytes;
        return result;
    }

    bool waitUntilIdle(std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mutex);
        return idle.wait_for(lock, timeout, [this] { return pending.empty() || !running.load(); });
    }

    void trimLocked() {
        if (entries.empty()) return;
        const int64_t oldestAllowed100ns = newestPts100ns - options.retention100ns;

        auto retireEntry = [&](const std::shared_ptr<Entry>& entry) {
            if (entry->retired.exchange(true, std::memory_order_relaxed)) return;
            if (entry->packet.payload) {
                residentPayloadBytes -= std::min<uint64_t>(
                    residentPayloadBytes,
                    static_cast<uint64_t>(entry->packet.payload->size()));
                if (residentPackets > 0) --residentPackets;
            } else if (entry->packet.payloadReader && diskBackedPackets > 0) {
                --diskBackedPackets;
            }
            if (entry->persistenceQueued) {
                queuedBytes -= std::min<uint64_t>(
                    queuedBytes,
                    static_cast<uint64_t>(entry->payloadBytes));
                if (entry->packet.payload) {
                    queuedResidentBytes -= std::min<uint64_t>(
                        queuedResidentBytes,
                        static_cast<uint64_t>(entry->payloadBytes));
                }
                if (queuedPackets > 0) --queuedPackets;
                entry->persistenceQueued = false;
            }
        };

        if (!options.alignSegmentsToKeyframes) {
            // Audio tracks can arrive slightly out of timestamp order. Remove
            // every expired packet instead of assuming a globally sorted deque.
            const std::size_t previousSpillCandidate = nextSpillCandidate;
            std::size_t originalIndex = 0;
            std::size_t removedBeforeSpillCandidate = 0;
            for (auto entry = entries.begin(); entry != entries.end();) {
                if ((*entry)->packet.pts100ns < oldestAllowed100ns) {
                    if (originalIndex < previousSpillCandidate) ++removedBeforeSpillCandidate;
                    retireEntry(*entry);
                    entry = entries.erase(entry);
                } else {
                    ++entry;
                }
                ++originalIndex;
            }
            nextSpillCandidate -= std::min(nextSpillCandidate, removedBeforeSpillCandidate);
            if (pending.empty()) idle.notify_all();
            return;
        }

        // Keep the latest decoder keyframe at or before the boundary. The mux
        // edit list hides this bounded preroll from the visible clip.
        std::size_t removeCount = 0;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& packet = entries[index]->packet;
            if (packet.pts100ns > oldestAllowed100ns) break;
            if (packet.kind == PacketKind::Video && packet.keyframe) removeCount = index;
        }
        while (removeCount > 0 && !entries.empty()) {
            --removeCount;
            auto entry = std::move(entries.front());
            entries.pop_front();
            if (nextSpillCandidate > 0) --nextSpillCandidate;
            retireEntry(entry);
        }
        if (pending.empty()) idle.notify_all();
    }
    void scheduleSpillsLocked() {
        uint64_t projectedResidentBytes = residentPayloadBytes -
            std::min(residentPayloadBytes, queuedResidentBytes);

        const uint64_t budget = options.residentPayloadBudgetBytes;
        if (projectedResidentBytes <= budget) return;

        while (projectedResidentBytes > budget && nextSpillCandidate < entries.size()) {
            const auto& entry = entries[nextSpillCandidate++];
            ++spillCandidateInspections;
            if (entry->retired.load(std::memory_order_relaxed) ||
                entry->persistenceQueued || !entry->packet.payload ||
                entry->packet.payload->empty()) {
                continue;
            }
            const auto bytes = static_cast<uint64_t>(entry->packet.payload->size());
            entry->persistenceQueued = true;
            pending.push_back(entry);
            ++queuedPackets;
            queuedBytes += bytes;
            queuedResidentBytes += bytes;
            projectedResidentBytes -= std::min(projectedResidentBytes, bytes);
        }
    }

    bool ensureSession() {
        if (session) return true;
        const auto root = options.rootDirectory.empty() ? defaultReplayRoot() : options.rootDirectory;
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error) return false;
        removeStaleSessions(root);

        static std::atomic<uint64_t> nextSession { 0 };
        const auto name = L"session-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(nextSession.fetch_add(1)) + L"-" + safeStreamName(options.streamName);
        auto next = std::make_shared<ReplaySessionDirectory>();
        next->path = root / name;
        next->deleteOnClose = options.deleteOnClose;
        std::filesystem::create_directories(next->path, error);
        if (error) return false;
        session = std::move(next);
        return true;
    }

    bool openSegment(int64_t firstPts100ns) {
        if (!ensureSession()) return false;
        auto backing = std::make_shared<SegmentBacking>();
        backing->session = session;
        backing->readCache = readCache;
        backing->path = session->path / (
            safeStreamName(options.streamName) + L"-" + std::to_wstring(nextSegment++) + L".bin");

        HANDLE handle = CreateFileW(
            backing->path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        applyLowIoPriority(handle);

        active.backing = std::move(backing);
        active.handle = handle;
        active.offset = 0;
        active.firstPts100ns = firstPts100ns;
        segments.push_back(active.backing);
        return true;
    }

    void closeActive() {
        if (!active.valid()) {
            active = {};
            return;
        }
        LARGE_INTEGER end {};
        end.QuadPart = static_cast<LONGLONG>(std::min<uint64_t>(
            active.offset,
            static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max())));
        if (SetFilePointerEx(active.handle, end, nullptr, FILE_BEGIN)) SetEndOfFile(active.handle);
        CloseHandle(active.handle);
        active.handle = INVALID_HANDLE_VALUE;
        active.backing->writtenBytes = active.offset;
        active = {};
    }

    bool shouldRotate(const EncodedPacket& packet) const {
        if (!active.valid() || active.offset == 0) return false;
        const bool targetReached =
            active.offset >= options.targetSegmentBytes ||
            packet.pts100ns - active.firstPts100ns >= options.targetSegmentDuration100ns;
        if (!targetReached) return false;
        return !options.alignSegmentsToKeyframes || packet.keyframe;
    }

    bool writeEntry(const std::shared_ptr<Entry>& entry) {
        const auto memory = entry->packet.payload;
        if (!memory || memory->empty()) return entry->packet.payloadReader != nullptr;

        {
            std::lock_guard lock(segmentMutex);
            if (rotationRequested.exchange(false, std::memory_order_relaxed) || shouldRotate(entry->packet)) {
                closeActive();
            }
            if (!active.valid() && !openSegment(entry->packet.pts100ns)) return false;
            const uint64_t payloadOffset = active.offset;
            std::span<const std::byte> remaining(memory->data(), memory->size());
            while (!remaining.empty()) {
                const DWORD request = static_cast<DWORD>(boundedWriteSize(
                    remaining.size(),
                    options.maximumWriteBytes));
                DWORD written = 0;
                if (!WriteFile(active.handle, remaining.data(), request, &written, nullptr) || written != request) {
                    return false;
                }
                maximumWriteSize = std::max<std::size_t>(maximumWriteSize, request);
                active.offset += written;
                active.backing->writtenBytes = active.offset;
                remaining = remaining.subspan(written);
            }

            auto reader = std::make_shared<SegmentPayloadReader>(
                active.backing,
                payloadOffset,
                memory->size());
            std::lock_guard entryLock(mutex);
            if (!entry->retired.load(std::memory_order_relaxed)) {
                entry->packet.payloadReader = std::move(reader);
                entry->packet.payload.reset();
                residentPayloadBytes -= std::min<uint64_t>(residentPayloadBytes, memory->size());
                queuedResidentBytes -= std::min<uint64_t>(queuedResidentBytes, memory->size());
                if (residentPackets > 0) --residentPackets;
                ++diskBackedPackets;
                ++persistedPackets;
                scheduleSpillsLocked();
            }
            healthy = true;
        }
        return true;
    }

    void discardCompletedPending(const std::shared_ptr<Entry>& entry) {
        {
            std::lock_guard lock(mutex);
            if (!pending.empty() && pending.front() == entry) {
                pending.pop_front();
            } else {
                const auto found = std::find(pending.begin(), pending.end(), entry);
                if (found != pending.end()) pending.erase(found);
            }
            if (entry->persistenceQueued) {
                queuedBytes -= std::min<uint64_t>(
                    queuedBytes,
                    static_cast<uint64_t>(entry->payloadBytes));
                if (entry->packet.payload) {
                    queuedResidentBytes -= std::min<uint64_t>(
                        queuedResidentBytes,
                        static_cast<uint64_t>(entry->payloadBytes));
                }
                if (queuedPackets > 0) --queuedPackets;
                entry->persistenceQueued = false;
            }
            if (pending.empty()) idle.notify_all();
        }
        std::lock_guard segmentLock(segmentMutex);
        cleanupSegments();
    }

    void cleanupSegments() {
        const auto activeBacking = active.backing;
        std::erase_if(segments, [&](const std::shared_ptr<SegmentBacking>& segment) {
            return segment != activeBacking && segment.use_count() == 1;
        });
    }

    void run() {
        const int previousPriority = GetThreadPriority(GetCurrentThread());
        const bool backgroundMode = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        while (true) {
            std::shared_ptr<Entry> entry;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] {
                    return stopRequested || !pending.empty() || rotationRequested.load(std::memory_order_relaxed);
                });
                if (stopRequested) break;
                if (rotationRequested.load(std::memory_order_relaxed) && pending.empty()) {
                    lock.unlock();
                    std::lock_guard segmentLock(segmentMutex);
                    closeActive();
                    rotationRequested.store(false, std::memory_order_relaxed);
                    cleanupSegments();
                    continue;
                }
                if (pending.empty()) continue;
                entry = pending.front();
            }

            if (entry->retired.load(std::memory_order_relaxed) || writeEntry(entry)) {
                discardCompletedPending(entry);
                continue;
            }

            {
                std::lock_guard lock(segmentMutex);
                healthy = false;
                ++writeFailures;
                closeActive();
                cleanupSegments();
            }
            std::unique_lock lock(mutex);
            wake.wait_for(lock, kRetryDelay, [this] { return stopRequested; });
            if (stopRequested) break;
        }

        {
            std::lock_guard lock(segmentMutex);
            closeActive();
            cleanupSegments();
        }
        if (backgroundMode) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        if (previousPriority != THREAD_PRIORITY_ERROR_RETURN) {
            SetThreadPriority(GetCurrentThread(), previousPriority);
        }
    }

    ReplaySegmentStoreOptions options;
    std::atomic<bool> running { false };
    mutable std::mutex mutex;
    mutable std::mutex segmentMutex;
    mutable std::condition_variable idle;
    std::condition_variable wake;
    std::thread worker;
    bool stopRequested = false;
    bool healthy = false;
    std::atomic<bool> rotationRequested { false };
    std::deque<std::shared_ptr<Entry>> entries;
    std::deque<std::shared_ptr<Entry>> pending;
    std::shared_ptr<ReplaySessionDirectory> session;
    std::shared_ptr<SegmentReadCache> readCache = std::make_shared<SegmentReadCache>();
    std::vector<std::shared_ptr<SegmentBacking>> segments;
    ActiveSegment active;
    uint64_t nextSegment = 0;
    std::size_t queuedPackets = 0;
    uint64_t queuedBytes = 0;
    uint64_t queuedResidentBytes = 0;
    uint64_t residentPayloadBytes = 0;
    std::size_t residentPackets = 0;
    std::size_t diskBackedPackets = 0;
    std::size_t nextSpillCandidate = 0;
    int64_t newestPts100ns = std::numeric_limits<int64_t>::min();
    uint64_t persistedPackets = 0;
    uint64_t spillCandidateInspections = 0;
    uint64_t writeFailures = 0;
    std::size_t maximumWriteSize = 0;
};

ReplaySegmentStore::ReplaySegmentStore(ReplaySegmentStoreOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

ReplaySegmentStore::~ReplaySegmentStore() = default;

void ReplaySegmentStore::start() { impl_->start(); }
void ReplaySegmentStore::stop() { impl_->stop(); }
void ReplaySegmentStore::setRetention(int64_t retention100ns) { impl_->setRetention(retention100ns); }
void ReplaySegmentStore::setResidentPayloadBudget(std::size_t bytes) {
    impl_->setResidentPayloadBudget(bytes);
}
void ReplaySegmentStore::push(const EncodedPacket& packet) { impl_->push(packet); }

std::vector<EncodedPacket> ReplaySegmentStore::selectWindow(
    int64_t startPts100ns,
    int64_t endPts100ns) const {
    return impl_->selectWindow(startPts100ns, endPts100ns);
}

std::vector<EncodedPacket> ReplaySegmentStore::snapshot() const { return impl_->snapshot(); }
void ReplaySegmentStore::clear() { impl_->clear(); }
std::size_t ReplaySegmentStore::size() const { return impl_->size(); }
ReplaySegmentStoreStats ReplaySegmentStore::stats() const { return impl_->stats(); }

bool ReplaySegmentStore::waitUntilIdle(std::chrono::milliseconds timeout) const {
    return impl_->waitUntilIdle(timeout);
}

}  // namespace clipture
