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

struct SegmentBacking {
    std::shared_ptr<ReplaySessionDirectory> session;
    std::filesystem::path path;
    std::atomic<uint64_t> writtenBytes { 0 };
    mutable std::mutex readMutex;
    mutable HANDLE readHandle = INVALID_HANDLE_VALUE;
    mutable uint64_t readFileOffset = 0;
    mutable bool readFileOffsetKnown = false;
    mutable uint64_t readAheadOffset = 0;
    mutable std::size_t readAheadSize = 0;
    mutable std::vector<std::byte> readAhead;

    ~SegmentBacking() {
        {
            std::lock_guard lock(readMutex);
            if (readHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(readHandle);
                readHandle = INVALID_HANDLE_VALUE;
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

        std::lock_guard lock(readMutex);
        if (readHandle == INVALID_HANDLE_VALUE) {
            readHandle = CreateFileW(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr);
            if (readHandle == INVALID_HANDLE_VALUE) return false;
            applyLowIoPriority(readHandle);
            readFileOffset = 0;
            readFileOffsetKnown = true;
            readAhead.reserve(kReadAheadBytes);
        }

        uint64_t currentOffset = offset;
        while (!destination.empty()) {
            if (currentOffset >= readAheadOffset && currentOffset - readAheadOffset < readAheadSize) {
                const std::size_t cacheOffset = static_cast<std::size_t>(currentOffset - readAheadOffset);
                const std::size_t copyBytes = std::min(destination.size(), readAheadSize - cacheOffset);
                std::memcpy(destination.data(), readAhead.data() + cacheOffset, copyBytes);
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

            if (!readFileOffsetKnown || readFileOffset != currentOffset) {
                LARGE_INTEGER position {};
                position.QuadPart = static_cast<LONGLONG>(currentOffset);
                if (!SetFilePointerEx(readHandle, position, nullptr, FILE_BEGIN)) {
                    readFileOffsetKnown = false;
                    return false;
                }
                readFileOffset = currentOffset;
                readFileOffsetKnown = true;
            }

            readAhead.resize(request);
            DWORD readBytes = 0;
            if (!ReadFile(
                    readHandle,
                    readAhead.data(),
                    static_cast<DWORD>(request),
                    &readBytes,
                    nullptr) || readBytes != request) {
                readFileOffsetKnown = false;
                readAheadSize = 0;
                return false;
            }
            readFileOffset += readBytes;
            readAheadOffset = currentOffset;
            readAheadSize = readBytes;
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
            queuedBytes = 0;
        }
        wake.notify_all();
        idle.notify_all();
        if (worker.joinable()) worker.join();
        {
            std::lock_guard lock(mutex);
            entries.clear();
            residentPayloadBytes = 0;
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
        const auto bytes = static_cast<uint64_t>(payloadSize(packet));
        {
            std::lock_guard lock(mutex);
            entries.push_back(entry);
            if (packet.payload) residentPayloadBytes += bytes;
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
        queuedBytes = 0;
        residentPayloadBytes = 0;
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
            result.queuedPackets = pending.size();
            result.queuedBytes = queuedBytes;
            result.residentPayloadBytes = residentPayloadBytes;
            result.residentPayloadBudgetBytes = options.residentPayloadBudgetBytes;
            result.persistedPackets = persistedPackets;
            for (const auto& entry : entries) {
                if (entry->packet.payload) ++result.residentPackets;
                if (!entry->packet.payload && entry->packet.payloadReader) {
                    ++result.diskBackedPackets;
                }
            }
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
        result.ramFallbackBytes = result.healthy ? 0 : result.queuedBytes;
        return result;
    }

    bool waitUntilIdle(std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mutex);
        return idle.wait_for(lock, timeout, [this] { return pending.empty() || !running.load(); });
    }

    void trimLocked() {
        if (entries.empty()) return;
        const int64_t newestPts100ns = entries.back()->packet.pts100ns;
        const int64_t oldestAllowed100ns = newestPts100ns - options.retention100ns;
        while (!entries.empty() && entries.front()->packet.pts100ns < oldestAllowed100ns) {
            auto entry = std::move(entries.front());
            entries.pop_front();
            entry->retired = true;
            if (entry->packet.payload) {
                residentPayloadBytes -= std::min<uint64_t>(
                    residentPayloadBytes,
                    static_cast<uint64_t>(entry->packet.payload->size()));
            }
            const auto pendingEntry = std::find(pending.begin(), pending.end(), entry);
            if (pendingEntry != pending.end()) {
                queuedBytes -= std::min<uint64_t>(
                    queuedBytes,
                    static_cast<uint64_t>(payloadSize(entry->packet)));
                pending.erase(pendingEntry);
                entry->persistenceQueued = false;
            }
        }
        if (pending.empty()) idle.notify_all();
    }

    void scheduleSpillsLocked() {
        uint64_t projectedResidentBytes = residentPayloadBytes;
        for (const auto& entry : entries) {
            if (!entry->persistenceQueued || !entry->packet.payload) continue;
            projectedResidentBytes -= std::min<uint64_t>(
                projectedResidentBytes,
                static_cast<uint64_t>(entry->packet.payload->size()));
        }

        const uint64_t budget = options.residentPayloadBudgetBytes;
        if (projectedResidentBytes <= budget) return;

        for (const auto& entry : entries) {
            if (projectedResidentBytes <= budget) break;
            if (entry->retired.load(std::memory_order_relaxed) ||
                entry->persistenceQueued || !entry->packet.payload ||
                entry->packet.payload->empty()) {
                continue;
            }
            const auto bytes = static_cast<uint64_t>(entry->packet.payload->size());
            entry->persistenceQueued = true;
            pending.push_back(entry);
            queuedBytes += bytes;
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
            const auto found = std::find(pending.begin(), pending.end(), entry);
            if (found != pending.end()) {
                pending.erase(found);
                queuedBytes -= std::min<uint64_t>(queuedBytes, payloadSize(entry->packet));
            }
            entry->persistenceQueued = false;
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
    std::vector<std::shared_ptr<SegmentBacking>> segments;
    ActiveSegment active;
    uint64_t nextSegment = 0;
    uint64_t queuedBytes = 0;
    uint64_t residentPayloadBytes = 0;
    uint64_t persistedPackets = 0;
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
