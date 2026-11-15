// Part of Mp4Muxer.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
StorageSeekPenalty queryStorageSeekPenalty(HANDLE handle) {
    if (handle == INVALID_HANDLE_VALUE) return StorageSeekPenalty::Unknown;
    STORAGE_PROPERTY_QUERY query {};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor {};
    DWORD returnedBytes = 0;
    if (!DeviceIoControl(
            handle,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            sizeof(query),
            &descriptor,
            sizeof(descriptor),
            &returnedBytes,
            nullptr) ||
        returnedBytes < sizeof(descriptor)) {
        return StorageSeekPenalty::Unknown;
    }
    return descriptor.IncursSeekPenalty
        ? StorageSeekPenalty::Incurs
        : StorageSeekPenalty::DoesNotIncur;
}

StorageSeekPenalty storageSeekPenaltyForPath(const std::wstring& path, HANDLE fileHandle) {
    const auto directResult = queryStorageSeekPenalty(fileHandle);
    if (directResult != StorageSeekPenalty::Unknown) return directResult;

    std::wstring fullPath(32'768, L'\0');
    const DWORD fullPathLength = GetFullPathNameW(
        path.c_str(),
        static_cast<DWORD>(fullPath.size()),
        fullPath.data(),
        nullptr);
    if (fullPathLength == 0 || fullPathLength >= fullPath.size()) return StorageSeekPenalty::Unknown;
    fullPath.resize(fullPathLength);

    std::wstring volumePath(32'768, L'\0');
    if (!GetVolumePathNameW(
            fullPath.c_str(),
            volumePath.data(),
            static_cast<DWORD>(volumePath.size()))) {
        return StorageSeekPenalty::Unknown;
    }
    volumePath.resize(volumePath.find(L'\0'));

    const UINT driveType = GetDriveTypeW(volumePath.c_str());
    if (driveType == DRIVE_RAMDISK) return StorageSeekPenalty::DoesNotIncur;
    if (driveType == DRIVE_REMOTE || driveType == DRIVE_CDROM || driveType == DRIVE_NO_ROOT_DIR) {
        return StorageSeekPenalty::Unknown;
    }

    std::wstring volumeName(32'768, L'\0');
    std::wstring volumeDevice;
    if (GetVolumeNameForVolumeMountPointW(
            volumePath.c_str(),
            volumeName.data(),
            static_cast<DWORD>(volumeName.size()))) {
        volumeName.resize(volumeName.find(L'\0'));
        while (!volumeName.empty() && (volumeName.back() == L'\\' || volumeName.back() == L'/')) {
            volumeName.pop_back();
        }
        volumeDevice = std::move(volumeName);
    } else if (volumePath.size() >= 2 && volumePath[1] == L':') {
        volumeDevice = L"\\\\.\\" + volumePath.substr(0, 2);
    } else {
        return StorageSeekPenalty::Unknown;
    }

    const HANDLE volumeHandle = CreateFileW(
        volumeDevice.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (volumeHandle == INVALID_HANDLE_VALUE) return StorageSeekPenalty::Unknown;
    const auto result = queryStorageSeekPenalty(volumeHandle);
    CloseHandle(volumeHandle);
    return result;
}

const char* storageSeekPenaltyName(StorageSeekPenalty value) {
    switch (value) {
        case StorageSeekPenalty::DoesNotIncur: return "none";
        case StorageSeekPenalty::Incurs: return "yes";
        default: return "unknown";
    }
}

const char* ioPriorityName(PRIORITY_HINT value) {
    switch (value) {
        case IoPriorityHintVeryLow: return "very-low";
        case IoPriorityHintLow: return "low";
        case IoPriorityHintNormal: return "normal";
        default: return "unknown";
    }
}

class SaveIoRecorder {
public:
    using Clock = std::chrono::steady_clock;

    SaveIoRecorder(
        bool enabled,
        SaveIoAnalysis& analysis,
        Clock::time_point startedAt)
        : enabled_(enabled),
          analysis_(analysis),
          startedAt_(startedAt) {
        analysis_ = {};
        analysis_.enabled = enabled_;
        analysis_.bucketMs = bucketMilliseconds_;
        if (enabled_) {
            haveStartingIoCounters_ =
                GetProcessIoCounters(GetCurrentProcess(), &startingIoCounters_) != FALSE;
        }
    }

    void setFileContext(
        const char* storageSeekPenalty,
        const char* ioPriority,
        bool lowIoPriorityApplied,
        bool preallocated,
        uint64_t finalFileBytes,
        uint64_t diskBackedSourceBytes) {
        if (!enabled_) return;
        analysis_.storageSeekPenalty = storageSeekPenalty;
        analysis_.ioPriority = ioPriority;
        analysis_.lowIoPriorityApplied = lowIoPriorityApplied;
        analysis_.preallocated = preallocated;
        analysis_.finalFileBytes = finalFileBytes;
        analysis_.diskBackedSourceBytes = diskBackedSourceBytes;
    }

    void recordReplayRead(
        Clock::time_point startedAt,
        uint64_t durationUs,
        uint64_t bytes) {
        if (!enabled_) return;
        auto& bucket = bucketFor(startedAt);
        bucket.replayReadBytes += bytes;
        bucket.replayReadBusyUs += durationUs;
        ++bucket.replayReadCalls;
        bucket.maximumReplayReadUs = std::max(bucket.maximumReplayReadUs, durationUs);
        recordSlowOperation("replay_read", startedAt, durationUs, bytes, false);
    }

    void recordOutputWrite(
        Clock::time_point startedAt,
        uint64_t durationUs,
        uint64_t bytes,
        const char* operation = "output_write") {
        if (!enabled_) return;
        auto& bucket = bucketFor(startedAt);
        bucket.outputWriteBytes += bytes;
        bucket.outputWriteBusyUs += durationUs;
        ++bucket.outputWriteCalls;
        bucket.maximumOutputWriteUs = std::max(bucket.maximumOutputWriteUs, durationUs);
        recordSlowOperation(operation, startedAt, durationUs, bytes, false);
    }

    void recordPacingWait(
        Clock::time_point startedAt,
        uint64_t durationUs,
        const char* operation) {
        if (!enabled_ || durationUs == 0) return;
        auto& bucket = bucketFor(startedAt);
        bucket.pacingWaitUs += durationUs;
        ++bucket.pacingWaitCalls;
        bucket.maximumPacingWaitUs = std::max(bucket.maximumPacingWaitUs, durationUs);
        recordSlowOperation(operation, startedAt, durationUs, 0, false);
    }

    void recordStructuralOperation(
        const char* operation,
        Clock::time_point startedAt,
        uint64_t durationUs,
        uint64_t bytes = 0) {
        if (!enabled_) return;
        recordSlowOperation(operation, startedAt, durationUs, bytes, true);
    }

    void recordPressure(Clock::time_point sampledAt, const MuxPressureSample& sample) {
        if (!enabled_) return;
        auto& bucket = bucketFor(sampledAt);
        bucket.pressureLevel = std::max(bucket.pressureLevel, static_cast<int>(sample.level));
        bucket.frameQueueDepth = std::max(bucket.frameQueueDepth, sample.frameQueueDepth);
        bucket.encoderQueueDepth = std::max(bucket.encoderQueueDepth, sample.encoderQueueDepth);
        bucket.nvencInFlight = std::max(bucket.nvencInFlight, sample.nvencInFlight);
        bucket.droppedFramesDelta = std::max(bucket.droppedFramesDelta, sample.droppedFramesDelta);
        bucket.captureGap100ns = std::max(bucket.captureGap100ns, sample.captureGap100ns);
        bucket.capturePublicationAge100ns = std::max(
            bucket.capturePublicationAge100ns,
            sample.capturePublicationAge100ns);
    }

    void finish() {
        if (!enabled_ || finished_) return;
        finished_ = true;
        analysis_.elapsedMs = static_cast<uint64_t>(std::max<int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - startedAt_).count()));

        IO_COUNTERS endingIoCounters {};
        if (haveStartingIoCounters_ &&
            GetProcessIoCounters(GetCurrentProcess(), &endingIoCounters) != FALSE) {
            analysis_.processReadOperationsDelta = positiveDelta(
                endingIoCounters.ReadOperationCount,
                startingIoCounters_.ReadOperationCount);
            analysis_.processWriteOperationsDelta = positiveDelta(
                endingIoCounters.WriteOperationCount,
                startingIoCounters_.WriteOperationCount);
            analysis_.processReadBytesDelta = positiveDelta(
                endingIoCounters.ReadTransferCount,
                startingIoCounters_.ReadTransferCount);
            analysis_.processWriteBytesDelta = positiveDelta(
                endingIoCounters.WriteTransferCount,
                startingIoCounters_.WriteTransferCount);
            analysis_.processOtherBytesDelta = positiveDelta(
                endingIoCounters.OtherTransferCount,
                startingIoCounters_.OtherTransferCount);
        }
    }

private:
    static constexpr uint32_t bucketMilliseconds_ = 50;
    static constexpr std::size_t maximumBuckets_ = 1200;
    static constexpr std::size_t maximumSlowOperations_ = 256;
    static constexpr uint64_t slowOperationThresholdUs_ = 8'000;

    static uint64_t positiveDelta(uint64_t current, uint64_t previous) {
        return current >= previous ? current - previous : 0;
    }

    uint64_t relativeMicroseconds(Clock::time_point timestamp) const {
        return static_cast<uint64_t>(std::max<int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::microseconds>(
                timestamp - startedAt_).count()));
    }

    SaveIoTimelineBucket& bucketFor(Clock::time_point timestamp) {
        const uint64_t offsetMs = relativeMicroseconds(timestamp) / 1000;
        std::size_t index = static_cast<std::size_t>(offsetMs / bucketMilliseconds_);
        if (index >= maximumBuckets_) {
            index = maximumBuckets_ - 1;
            analysis_.timelineTruncated = true;
        }
        if (analysis_.timeline.size() <= index) {
            const std::size_t previousSize = analysis_.timeline.size();
            analysis_.timeline.resize(index + 1);
            for (std::size_t i = previousSize; i <= index; ++i) {
                analysis_.timeline[i].startMs =
                    static_cast<uint32_t>(i * bucketMilliseconds_);
            }
        }
        return analysis_.timeline[index];
    }

    void recordSlowOperation(
        const char* operation,
        Clock::time_point startedAt,
        uint64_t durationUs,
        uint64_t bytes,
        bool force) {
        if (!force && durationUs < slowOperationThresholdUs_) return;
        if (analysis_.slowOperations.size() >= maximumSlowOperations_) {
            ++analysis_.omittedSlowOperations;
            return;
        }
        analysis_.slowOperations.push_back({
            operation,
            static_cast<uint32_t>(std::min<uint64_t>(
                relativeMicroseconds(startedAt) / 1000,
                std::numeric_limits<uint32_t>::max())),
            durationUs,
            bytes
        });
    }

    bool enabled_ = false;
    bool finished_ = false;
    bool haveStartingIoCounters_ = false;
    SaveIoAnalysis& analysis_;
    Clock::time_point startedAt_;
    IO_COUNTERS startingIoCounters_ {};
};

class Win32FileWriter {
public:
    static constexpr DWORD maxWriteBytes = 512u * 1024u;

    explicit Win32FileWriter(const std::wstring& path) {
        handle_ = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_NEW, // Refuse a collision even if a file appeared after name selection.
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            lastError_ = GetLastError();
            return;
        }

        storageSeekPenalty_ = storageSeekPenaltyForPath(path, handle_);

        FILE_IO_PRIORITY_HINT_INFO priorityInfo {};
        priorityInfo.PriorityHint = storageSeekPenalty_ == StorageSeekPenalty::DoesNotIncur
            ? IoPriorityHintLow
            : IoPriorityHintVeryLow;
        ioPriorityHint_ = priorityInfo.PriorityHint;
        lowPriorityApplied_ = SetFileInformationByHandle(
            handle_,
            FileIoPriorityHintInfo,
            &priorityInfo,
            sizeof(priorityInfo)) != FALSE;
    }

    ~Win32FileWriter() {
        close();
    }

    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    bool good() const { return valid() && lastError_ == ERROR_SUCCESS; }
    bool preallocated() const { return preallocated_; }
    bool lowPriorityApplied() const { return lowPriorityApplied_; }
    PRIORITY_HINT ioPriorityHint() const { return ioPriorityHint_; }
    StorageSeekPenalty storageSeekPenalty() const { return storageSeekPenalty_; }
    uint64_t bytesWritten() const { return bytesWritten_; }
    DWORD maximumWriteSize() const { return maximumWriteSize_; }
    DWORD lastError() const { return lastError_; }

    bool preallocate(uint64_t size) {
        if (!good() || size > static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max())) return false;
        LARGE_INTEGER target {};
        target.QuadPart = static_cast<LONGLONG>(size);
        LARGE_INTEGER beginning {};
        FILE_ALLOCATION_INFO allocation {};
        allocation.AllocationSize = target;
        const bool allocationReserved = SetFileInformationByHandle(
            handle_,
            FileAllocationInfo,
            &allocation,
            sizeof(allocation)) != FALSE;
        if (!SetFilePointerEx(handle_, target, nullptr, FILE_BEGIN) || !SetEndOfFile(handle_)) {
            SetFilePointerEx(handle_, beginning, nullptr, FILE_BEGIN);
            return false;
        }
        if (!SetFilePointerEx(handle_, beginning, nullptr, FILE_BEGIN)) return false;
        preallocated_ = allocationReserved;
        return preallocated_;
    }

    bool write(std::span<const std::byte> bytes) {
        while (!bytes.empty() && good()) {
            const DWORD request = static_cast<DWORD>(boundedWriteSize(bytes.size(), maxWriteBytes));
            DWORD written = 0;
            if (!WriteFile(handle_, bytes.data(), request, &written, nullptr) || written != request) {
                lastError_ = GetLastError();
                if (lastError_ == ERROR_SUCCESS) lastError_ = ERROR_WRITE_FAULT;
                return false;
            }
            maximumWriteSize_ = std::max(maximumWriteSize_, request);
            bytesWritten_ += written;
            bytes = bytes.subspan(written);
        }
        return good();
    }

    bool close() {
        if (handle_ == INVALID_HANDLE_VALUE) return lastError_ == ERROR_SUCCESS;
        const HANDLE handle = std::exchange(handle_, INVALID_HANDLE_VALUE);
        if (!CloseHandle(handle)) {
            lastError_ = GetLastError();
            return false;
        }
        return lastError_ == ERROR_SUCCESS;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    DWORD lastError_ = ERROR_SUCCESS;
    DWORD maximumWriteSize_ = 0;
    uint64_t bytesWritten_ = 0;
    bool preallocated_ = false;
    bool lowPriorityApplied_ = false;
    PRIORITY_HINT ioPriorityHint_ = IoPriorityHintNormal;
    StorageSeekPenalty storageSeekPenalty_ = StorageSeekPenalty::Unknown;
};

void writeU32(Win32FileWriter& out, uint32_t value) {
    const std::byte bytes[] = {
        static_cast<std::byte>((value >> 24) & 0xFF),
        static_cast<std::byte>((value >> 16) & 0xFF),
        static_cast<std::byte>((value >> 8) & 0xFF),
        static_cast<std::byte>(value & 0xFF)
    };
    out.write(bytes);
}

void writeU64(Win32FileWriter& out, uint64_t value) {
    writeU32(out, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFULL));
    writeU32(out, static_cast<uint32_t>(value & 0xFFFFFFFFULL));
}

void writeType(Win32FileWriter& out, const char type[4]) {
    out.write(std::as_bytes(std::span<const char>(type, 4)));
}

void writeBytes(Win32FileWriter& out, const Bytes& bytes) {
    out.write(std::as_bytes(std::span<const uint8_t>(bytes.data(), bytes.size())));
}

class BufferedByteWriter {
public:
    explicit BufferedByteWriter(
        Win32FileWriter& out,
        std::size_t capacity = 4 * 1024 * 1024,
        std::function<MuxPressureSample()> samplePressure = {},
        bool adaptiveRateEnabled = false,
        AdaptiveWritePacerConfig adaptiveRate = {},
        std::size_t burstBytes = 0,
        SaveIoRecorder* ioRecorder = nullptr)
        : out_(out),
          capacity_(std::max<std::size_t>(capacity, 1)),
          samplePressure_(std::move(samplePressure)),
          adaptiveRateEnabled_(adaptiveRateEnabled),
          rateController_(adaptiveRate),
          burstBytes_(adaptiveRateEnabled
              ? std::max<std::size_t>(burstBytes, ioChunkBytes_)
              : 0),
          ioRecorder_(ioRecorder) {
        buffer_.reserve(capacity_);
    }

    ~BufferedByteWriter() {
        finishBackgroundMode();
    }

    void write(std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
            const std::size_t available = capacity_ - buffer_.size();
            if (available == 0) {
                flush();
                continue;
            }

            const std::size_t count = std::min(available, bytes.size());
            buffer_.insert(buffer_.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count));
            bytes = bytes.subspan(count);
        }
    }

    bool writeFromReader(
        const PacketPayloadReader& reader,
        std::size_t offset,
        std::size_t size) {
        while (size > 0) {
            const std::size_t available = capacity_ - buffer_.size();
            if (available == 0) {
                flush();
                continue;
            }

            const std::size_t count = std::min({ available, size, sourceReadChunkBytes_ });
            const std::size_t previousSize = buffer_.size();
            buffer_.resize(previousSize + count);
            // Replay reads and final MP4 writes compete for the same storage,
            // so both obey capture pressure and consume one aggregate I/O budget.
            prepareForIo(count);
            const auto readStartedAt = std::chrono::steady_clock::now();
            const bool read = reader.read(
                offset,
                std::span<std::byte>(buffer_.data() + previousSize, count));
            const auto readUs = static_cast<uint64_t>(std::max<int64_t>(
                0,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - readStartedAt).count()));
            ++sourceReadCalls_;
            sourceReadUs_ += readUs;
            maximumSourceReadUs_ = std::max(maximumSourceReadUs_, readUs);
            if (ioRecorder_) {
                ioRecorder_->recordReplayRead(readStartedAt, readUs, read ? count : 0);
            }
            if (!read) {
                buffer_.resize(previousSize);
                return false;
            }
            if (adaptiveRateEnabled_) rateController_.observeIoLatency(readUs);
            sourceReadBytes_ += count;
            offset += count;
            size -= count;
        }
        return true;
    }

    void flush() {
        if (buffer_.empty()) return;
        std::size_t offset = 0;
        while (offset < buffer_.size()) {
            const std::size_t count = std::min(ioChunkBytes_, buffer_.size() - offset);
            prepareForIo(count);
            const auto writeStartedAt = std::chrono::steady_clock::now();
            const bool wrote = out_.write(std::span<const std::byte>(buffer_.data() + offset, count));
            const auto writeDurationUs = static_cast<uint64_t>(std::max<int64_t>(
                0,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - writeStartedAt).count()));
            ++outputWriteCalls_;
            outputWriteUs_ += writeDurationUs;
            maximumOutputWriteUs_ = std::max(maximumOutputWriteUs_, writeDurationUs);
            if (ioRecorder_) {
                ioRecorder_->recordOutputWrite(writeStartedAt, writeDurationUs, wrote ? count : 0);
            }
            if (!wrote) break;
            if (adaptiveRateEnabled_) rateController_.observeWrite(count, writeDurationUs);
            offset += count;
        }
        buffer_.clear();
        ++flushes_;
    }

    std::size_t flushCount() const {
        return flushes_;
    }

    std::size_t sleepCount() const {
        return sleeps_;
    }

    std::size_t sleepMs() const {
        return sleepMs_;
    }

    std::size_t yieldCount() const { return yields_; }
    std::size_t recoveryWaitCount() const { return recoveryWaits_; }
    std::size_t recoveryWaitMs() const { return recoveryWaitMs_; }
    bool adaptiveRateEnabled() const { return adaptiveRateEnabled_; }
    uint64_t rateLimitBytesPerSecond() const {
        return adaptiveRateEnabled_ ? rateController_.currentBytesPerSecond() : 0;
    }
    uint64_t minimumRateBytesPerSecond() const {
        return adaptiveRateEnabled_ ? rateController_.minimumRateSeen() : 0;
    }
    uint64_t dynamicMinimumRateBytesPerSecond() const {
        return adaptiveRateEnabled_ ? rateController_.dynamicMinimumRate() : 0;
    }
    uint64_t maximumRateBytesPerSecond() const {
        return adaptiveRateEnabled_ ? rateController_.maximumRateSeen() : 0;
    }
    uint64_t observedWriteBytesPerSecond() const {
        return adaptiveRateEnabled_ ? rateController_.observedServiceBytesPerSecond() : 0;
    }
    std::size_t rateAdjustments() const {
        return adaptiveRateEnabled_ ? rateController_.rateAdjustments() : 0;
    }
    std::size_t pressureBackoffs() const {
        return adaptiveRateEnabled_ ? rateController_.pressureBackoffs() : 0;
    }
    std::size_t latencyBackoffs() const {
        return adaptiveRateEnabled_ ? rateController_.latencyBackoffs() : 0;
    }
    std::size_t ignoredLatencyStalls() const {
        return adaptiveRateEnabled_ ? rateController_.ignoredLatencyStalls() : 0;
    }
    std::size_t measuredWrites() const {
        return adaptiveRateEnabled_ ? rateController_.measuredWrites() : 0;
    }
    std::size_t adaptiveBackgroundEntries() const { return adaptiveBackgroundEntries_; }
    std::size_t adaptiveBackgroundMs() const {
        uint64_t totalUs = adaptiveBackgroundUs_;
        if (adaptiveBackgroundStarted_) {
            totalUs += steadyNowUs() - adaptiveBackgroundStartedAtUs_;
        }
        return static_cast<std::size_t>(totalUs / 1000);
    }
    uint64_t sourceReadBytes() const { return sourceReadBytes_; }
    std::size_t sourceReadCalls() const { return sourceReadCalls_; }
    std::size_t sourceReadMs() const { return static_cast<std::size_t>(sourceReadUs_ / 1000); }
    uint64_t maximumSourceReadUs() const { return maximumSourceReadUs_; }
    std::size_t outputWriteCalls() const { return outputWriteCalls_; }
    std::size_t outputWriteMs() const { return static_cast<std::size_t>(outputWriteUs_ / 1000); }
    uint64_t maximumOutputWriteUs() const { return maximumOutputWriteUs_; }
    std::size_t rateLimitSleepCount() const { return rateLimitSleeps_; }
    std::size_t rateLimitSleepMs() const { return rateLimitSleepUs_ / 1000; }
    std::size_t pressureTransitions() const { return pressureTransitions_; }
    int64_t maximumQueueAge100ns() const { return maximumQueueAge100ns_; }
    int maximumEncoderQueueDepth() const { return maximumEncoderQueueDepth_; }
    int maximumNvencInFlight() const { return maximumNvencInFlight_; }
    int64_t maximumCaptureGap100ns() const { return maximumCaptureGap100ns_; }
    int64_t maximumCapturePublicationAge100ns() const { return maximumCapturePublicationAge100ns_; }

private:
    static constexpr std::size_t ioChunkBytes_ = 512u * 1024u;
    static constexpr std::size_t sourceReadChunkBytes_ = 2u * 1024u * 1024u;

    static uint64_t steadyNowUs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    static AdaptiveWritePressure adaptivePressure(MuxPressureLevel level) {
        switch (level) {
            case MuxPressureLevel::Critical: return AdaptiveWritePressure::Critical;
            case MuxPressureLevel::Elevated: return AdaptiveWritePressure::Elevated;
            default: return AdaptiveWritePressure::Healthy;
        }
    }

    void finishBackgroundMode() {
        if (!adaptiveBackgroundStarted_) return;
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        adaptiveBackgroundUs_ += steadyNowUs() - adaptiveBackgroundStartedAtUs_;
        adaptiveBackgroundStarted_ = false;
    }

    void updateBackgroundMode(AdaptiveWritePressure pressure) {
        if (pressure == AdaptiveWritePressure::Healthy) {
            finishBackgroundMode();
            adaptiveBackgroundAttempted_ = false;
            return;
        }
        if (pressure == AdaptiveWritePressure::Elevated) return;
        if (adaptiveBackgroundStarted_ || adaptiveBackgroundAttempted_) return;
        adaptiveBackgroundAttempted_ = true;
        if (SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE) {
            adaptiveBackgroundStarted_ = true;
            adaptiveBackgroundStartedAtUs_ = steadyNowUs();
            ++adaptiveBackgroundEntries_;
        }
    }

    MuxPressureSample observePressure() {
        const auto sample = samplePressure_();
        if (ioRecorder_) ioRecorder_->recordPressure(std::chrono::steady_clock::now(), sample);
        maximumQueueAge100ns_ = std::max(maximumQueueAge100ns_, sample.oldestFrameAge100ns);
        maximumEncoderQueueDepth_ = std::max(maximumEncoderQueueDepth_, sample.encoderQueueDepth);
        maximumNvencInFlight_ = std::max(maximumNvencInFlight_, sample.nvencInFlight);
        maximumCaptureGap100ns_ = std::max(maximumCaptureGap100ns_, sample.captureGap100ns);
        maximumCapturePublicationAge100ns_ = std::max(
            maximumCapturePublicationAge100ns_,
            sample.capturePublicationAge100ns);
        if (sample.level != pressure_) {
            pressure_ = sample.level;
            ++pressureTransitions_;
        }
        const auto sustainedPressure = pressureGate_.update(
            adaptivePressure(sample.level),
            steadyNowUs());
        if (adaptiveRateEnabled_) rateController_.observePressure(sustainedPressure);
        updateBackgroundMode(sustainedPressure);
        return sample;
    }

    void paceElevated(std::size_t pendingBytes) {
        elevatedBytesSinceYield_ += pendingBytes;
        if (elevatedBytesSinceYield_ < elevatedYieldIntervalBytes_) return;
        SwitchToThread();
        ++yields_;
        elevatedBytesSinceYield_ %= elevatedYieldIntervalBytes_;
    }

    void prepareForIo(std::size_t pendingBytes) {
        if (samplePressure_) {
            auto sample = observePressure();
            if (sample.level == MuxPressureLevel::Elevated) {
                paceElevated(pendingBytes);
            } else if (sample.level == MuxPressureLevel::Critical) {
                elevatedBytesSinceYield_ = 0;
                ++recoveryWaits_;
                const auto waitStartedAt = std::chrono::steady_clock::now();
                SwitchToThread();
                ++yields_;
                sample = observePressure();
                const auto waitedUs = static_cast<uint64_t>(std::max<int64_t>(
                    0,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - waitStartedAt).count()));
                recoveryWaitMs_ += static_cast<std::size_t>(waitedUs / 1000);
                if (ioRecorder_) {
                    ioRecorder_->recordPacingWait(
                        waitStartedAt,
                        waitedUs,
                        "capture_pressure_wait");
                }
                if (sample.level == MuxPressureLevel::Elevated) {
                    paceElevated(pendingBytes);
                }
            } else {
                elevatedBytesSinceYield_ = 0;
            }
        }
        paceToTarget(pendingBytes);
    }

    void paceToTarget(std::size_t pendingBytes) {
        const uint64_t bytesPerSecond = rateLimitBytesPerSecond();
        if (bytesPerSecond == 0 || pendingBytes == 0) return;

        using Clock = std::chrono::steady_clock;
        const auto now = Clock::now();
        const auto burstDuration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(
                static_cast<double>(burstBytes_) / static_cast<double>(bytesPerSecond)));
        const auto writeDuration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(
                static_cast<double>(pendingBytes) / static_cast<double>(bytesPerSecond)));

        if (!rateLimitStarted_) {
            nextRateLimitedIo_ = now - burstDuration;
            rateLimitStarted_ = true;
        } else {
            nextRateLimitedIo_ = std::max(nextRateLimitedIo_, now - burstDuration);
        }
        nextRateLimitedIo_ += writeDuration;
        if (nextRateLimitedIo_ <= now) return;

        const auto sleepStartedAt = Clock::now();
        std::this_thread::sleep_until(nextRateLimitedIo_);
        const auto sleptUs = std::max<int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - sleepStartedAt).count());
        ++rateLimitSleeps_;
        rateLimitSleepUs_ += static_cast<std::size_t>(sleptUs);
        if (ioRecorder_) {
            ioRecorder_->recordPacingWait(
                sleepStartedAt,
                static_cast<uint64_t>(sleptUs),
                "rate_limit_wait");
        }
    }

    Win32FileWriter& out_;
    const std::size_t capacity_;
    std::function<MuxPressureSample()> samplePressure_;
    const bool adaptiveRateEnabled_;
    AdaptiveWriteRateController rateController_;
    SustainedWritePressureGate pressureGate_;
    const std::size_t burstBytes_;
    SaveIoRecorder* ioRecorder_ = nullptr;
    std::vector<std::byte> buffer_;
    std::size_t flushes_ = 0;
    std::size_t sleeps_ = 0;
    std::size_t sleepMs_ = 0;
    std::size_t yields_ = 0;
    std::size_t recoveryWaits_ = 0;
    std::size_t recoveryWaitMs_ = 0;
    std::size_t rateLimitSleeps_ = 0;
    std::size_t rateLimitSleepUs_ = 0;
    std::size_t pressureTransitions_ = 0;
    std::size_t adaptiveBackgroundEntries_ = 0;
    uint64_t adaptiveBackgroundUs_ = 0;
    uint64_t adaptiveBackgroundStartedAtUs_ = 0;
    uint64_t sourceReadBytes_ = 0;
    uint64_t sourceReadUs_ = 0;
    uint64_t maximumSourceReadUs_ = 0;
    uint64_t outputWriteUs_ = 0;
    uint64_t maximumOutputWriteUs_ = 0;
    std::size_t sourceReadCalls_ = 0;
    std::size_t outputWriteCalls_ = 0;
    int64_t maximumQueueAge100ns_ = 0;
    int maximumEncoderQueueDepth_ = 0;
    int maximumNvencInFlight_ = 0;
    int64_t maximumCaptureGap100ns_ = 0;
    int64_t maximumCapturePublicationAge100ns_ = 0;
    MuxPressureLevel pressure_ = MuxPressureLevel::Healthy;
    bool adaptiveBackgroundStarted_ = false;
    bool adaptiveBackgroundAttempted_ = false;
    bool rateLimitStarted_ = false;
    std::chrono::steady_clock::time_point nextRateLimitedIo_ {};
    std::size_t elevatedBytesSinceYield_ = 0;
    static constexpr std::size_t elevatedYieldIntervalBytes_ = 4u * 1024u * 1024u;
};

void writeU32(BufferedByteWriter& out, uint32_t value) {
    const std::byte bytes[4] = {
        static_cast<std::byte>((value >> 24) & 0xFF),
        static_cast<std::byte>((value >> 16) & 0xFF),
        static_cast<std::byte>((value >> 8) & 0xFF),
        static_cast<std::byte>(value & 0xFF)
    };
    out.write(std::span<const std::byte>(bytes, 4));
}

void writeBytes(BufferedByteWriter& out, std::span<const std::byte> bytes) {
    out.write(bytes);
}

void writeBytes(BufferedByteWriter& out, const Bytes& bytes) {
    writeBytes(out, std::as_bytes(std::span<const uint8_t>(bytes.data(), bytes.size())));
}

void writeBytes(BufferedByteWriter& out, const std::vector<std::byte>& bytes) {
    writeBytes(out, std::span<const std::byte>(bytes.data(), bytes.size()));
}

bool writePacketRange(
    BufferedByteWriter& out,
    const EncodedPacket& packet,
    std::size_t offset,
    std::size_t size) {
    if (offset > payloadSize(packet) || size > payloadSize(packet) - offset) return false;
    if (size == 0) return true;
    if (packet.payload) {
        writeBytes(out, std::span<const std::byte>(packet.payload->data() + offset, size));
        return true;
    }
    if (!packet.payloadReader) return false;
    return out.writeFromReader(*packet.payloadReader, offset, size);
}

bool writeSamplePayload(
    BufferedByteWriter& out,
    const OwnedSample& sample) {
    const auto memory = samplePayload(sample);
    if (!memory.empty()) {
        writeBytes(out, memory);
        return true;
    }
    if (!sample.payloadReader) return sample.info.size == 0;
    return out.writeFromReader(*sample.payloadReader, 0, sample.info.size);
}

bool writeAvccSample(
    BufferedByteWriter& out,
    const VideoSamplePlan& sample) {
    if (!sample.packet) return false;
    if (sample.packet->codec == PacketCodec::H264Avcc) {
        return writePacketRange(out, *sample.packet, 0, sample.info.size);
    }
    for (const auto& nalu : sample.writableNalus) {
        writeU32(out, static_cast<uint32_t>(nalu.size));
        if (!writePacketRange(out, *sample.packet, nalu.offset, nalu.size)) return false;
    }
    return true;
}

