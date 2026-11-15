// Part of EncoderWorker.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
EncoderWorker::EncoderWorker(
    FrameQueue& frames,
    PacketRingBuffer& packets,
    ReplaySegmentStore* replayStore)
    : frames_(frames),
      packets_(packets),
      replayStore_(replayStore) {}

EncoderWorker::~EncoderWorker() {
    stop();
}

void EncoderWorker::start() {
    if (running_.exchange(true)) return;
    recentSchedulerWakeLateness_.clear();

    recentInputPreparationLatency_.clear();
    recentQueueResidenceLatency_.clear();
    recentInputPtsInterval_.clear();
    recentOutputPtsInterval_.clear();
    recentDistinctSourceOutputEvents_.clear();
    recentRepeatedSourceOutputEvents_.clear();
    recentUnknownSourceOutputEvents_.clear();
    recentInputMapLatency_.clear();
    recentNvencCallLatency_.clear();
    recentOutputEventWaitLatency_.clear();
    recentOutputLockLatency_.clear();
    recentOutputCopyLatency_.clear();
    recentOutputUnmapLatency_.clear();
    performanceStarted100ns_.store(monotonicNow100ns(), std::memory_order_relaxed);

    HMODULE module = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!module) module = LoadLibraryW(L"nvEncodeAPI.dll");
    nvencRuntimeLoaded_ = module && GetProcAddress(module, "NvEncodeAPICreateInstance");
    if (module) FreeLibrary(module);

    setStatus(nvencRuntimeLoaded_
        ? "NVENC runtime loaded; capture frames are being handed to the encoder worker."
        : "NVENC runtime was not found; encoder worker cannot produce H.264 packets.");
    encodeThread_ = std::thread(&EncoderWorker::encodeLoop, this);
    thread_ = std::thread(&EncoderWorker::run, this);
}

void EncoderWorker::stop() {
    if (!running_.exchange(false)) return;
    frames_.stop();
    {
        std::lock_guard lock(submitMutex_);
        pendingJobs_.clear();
        pendingOutputFrames_ = 0;
        pendingFreshFrames_ = 0;
        pendingRepeatFrames_ = 0;
    }
    submitCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    submitCv_.notify_all();
    if (encodeThread_.joinable()) encodeThread_.join();
}

bool EncoderWorker::running() const {
    return running_;
}

void EncoderWorker::configure(
    int fps,
    int bitrateMbps,
    int targetWidth,
    int targetHeight,
    int maxEncodeWidth,
    int maxEncodeHeight,
    int nvencPreset,
    bool discardBufferedPackets) {
    const int nextFps = std::clamp(fps, 24, 240);
    const int nextBitrateMbps = std::clamp(bitrateMbps, 4, 120);
    const int nextTargetWidth = std::max(0, targetWidth);
    const int nextTargetHeight = std::max(0, targetHeight);
    const int nextMaxEncodeWidth = std::max(0, maxEncodeWidth);
    const int nextMaxEncodeHeight = std::max(0, maxEncodeHeight);
    const int nextPreset = std::clamp(nvencPreset, 1, 5);

    const bool changed =
        targetFps_.load() != nextFps ||
        targetBitrateMbps_.load() != nextBitrateMbps ||
        targetWidth_.load() != nextTargetWidth ||
        targetHeight_.load() != nextTargetHeight ||
        maxEncodeWidth_.load() != nextMaxEncodeWidth ||
        maxEncodeHeight_.load() != nextMaxEncodeHeight ||
        nvencPreset_.load() != nextPreset;

    targetFps_ = nextFps;
    targetBitrateMbps_ = nextBitrateMbps;
    targetWidth_ = nextTargetWidth;
    targetHeight_ = nextTargetHeight;
    maxEncodeWidth_ = nextMaxEncodeWidth;
    maxEncodeHeight_ = nextMaxEncodeHeight;
    nvencPreset_ = nextPreset;
    if (changed) {
        const int nextVersion = configVersion_.fetch_add(1) + 1;
        if (discardBufferedPackets) discardPacketsAtConfigVersion_.store(nextVersion);
    }
}

void EncoderWorker::requireFreshFrame(bool discardBufferedPackets) {
    frames_.clear();
    const int nextVersion = freshFrameVersion_.fetch_add(1) + 1;
    if (discardBufferedPackets) discardPacketsAtFreshFrameVersion_.store(nextVersion);
}

void EncoderWorker::resetAutoOutputResolution() {
    autoOutputWidth_ = 0;
    autoOutputHeight_ = 0;
    requireFreshFrame(true);
}

bool EncoderWorker::nvencRuntimeLoaded() const {
    return nvencRuntimeLoaded_;
}

int EncoderWorker::framesAccepted() const {
    return framesAccepted_;
}

int EncoderWorker::framesEncoded() const {
    return framesEncoded_;
}

uint64_t EncoderWorker::encoderSetups() const {
    return encoderSetups_.load();
}

long EncoderWorker::deviceRemovedHr() const {
    return deviceRemovedHr_.load(std::memory_order_acquire);
}

void EncoderWorker::setSetupObserver(std::function<void()> observer) {
    setupObserver_ = std::move(observer);
}

int EncoderWorker::queuedEncodeFrames() const {
    std::lock_guard lock(submitMutex_);
    return static_cast<int>(std::min<std::size_t>(pendingOutputFrames_, INT_MAX));
}

int EncoderWorker::queuedFreshEncodeFrames() const {
    std::lock_guard lock(submitMutex_);
    return static_cast<int>(std::min<std::size_t>(pendingFreshFrames_, INT_MAX));
}

int EncoderWorker::queuedRepeatEncodeFrames() const {
    std::lock_guard lock(submitMutex_);
    return static_cast<int>(std::min<std::size_t>(pendingRepeatFrames_, INT_MAX));
}

int EncoderWorker::schedulerDroppedFrames() const {
    return schedulerDroppedFrames_.load();
}

int EncoderWorker::schedulerRepeatedFrames() const {
    return schedulerRepeatedFrames_.load();
}

uint64_t EncoderWorker::presentLatchWaits() const {
    return presentLatchWaits_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::presentLatchHits() const {
    return presentLatchHits_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::presentLatchTimeouts() const {
    return presentLatchTimeouts_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::catchUpEvents() const {
    return catchUpEvents_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::historicalFramesRecovered() const {
    return historicalFramesRecovered_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::catchUpRepeatedTicks() const {
    return catchUpRepeatedTicks_.load(std::memory_order_relaxed);
}

bool EncoderWorker::stillFrameDuplicationEnabled() const {
    return kEnableFrameDuplication;
}

const SlotPoolUsage& EncoderWorker::nvencOutputSlots() const {
    return nvencOutputSlots_;
}

int EncoderWorker::encoderQueueDrops() const {
    return encoderQueueDrops_.load();
}

int EncoderWorker::encoderRepeatCoalesced() const {
    return encoderRepeatCoalesced_.load();
}

uint64_t EncoderWorker::encoderAdmissionRejections() const {
    return encoderAdmissionRejections_.load(std::memory_order_relaxed);
}

int EncoderWorker::nvencSurfaceDrops() const {
    return nvencSurfaceDrops_.load();
}

int EncoderWorker::nvencInputDrops() const {
    return nvencInputDrops_.load();
}

int EncoderWorker::encoderBackpressureDrops() const {
    return encoderBackpressureDrops_.load();
}

int EncoderWorker::nvencInFlightFrames() const {
    return nvencInFlightFrames_.load();
}

uint64_t EncoderWorker::distinctSourceOutputFrames() const {
    return distinctSourceOutputFrames_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::repeatedSourceOutputFrames() const {
    return repeatedSourceOutputFrames_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::unknownSourceOutputFrames() const {
    return unknownSourceOutputFrames_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::motionFramesTotal() const {
    return motionFramesTotal_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::motionFramesRepeated() const {
    return motionFramesRepeated_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::nvencZeroCopyFrames() const {
    return nvencZeroCopyFrames_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::nvencCopyFallbackFrames() const {
    return nvencCopyFallbackFrames_.load(std::memory_order_relaxed);
}

uint64_t EncoderWorker::nvencConvertedFrames() const {
    return nvencConvertedFrames_.load(std::memory_order_relaxed);
}

int EncoderWorker::effectiveNvencPreset() const {
    return nvencPreset_.load(std::memory_order_relaxed);
}

int64_t EncoderWorker::maximumSubmitLatency100ns() const {
    return maximumSubmitLatency100ns_.load();
}

int64_t EncoderWorker::averageScaleLatency100ns() const {
    const uint64_t count = profiledSubmissions_.load(std::memory_order_relaxed);
    return count > 0
        ? totalScaleLatency100ns_.load(std::memory_order_relaxed) / static_cast<int64_t>(count)
        : 0;
}

int64_t EncoderWorker::maximumScaleLatency100ns() const {
    return maximumScaleLatency100ns_.load(std::memory_order_relaxed);
}

int64_t EncoderWorker::averageInputMapLatency100ns() const {
    const uint64_t count = profiledSubmissions_.load(std::memory_order_relaxed);
    return count > 0
        ? totalInputMapLatency100ns_.load(std::memory_order_relaxed) / static_cast<int64_t>(count)
        : 0;
}

int64_t EncoderWorker::maximumInputMapLatency100ns() const {
    return maximumInputMapLatency100ns_.load(std::memory_order_relaxed);
}

int64_t EncoderWorker::averageNvencCallLatency100ns() const {
    const uint64_t count = profiledSubmissions_.load(std::memory_order_relaxed);
    return count > 0
        ? totalNvencCallLatency100ns_.load(std::memory_order_relaxed) / static_cast<int64_t>(count)
        : 0;
}

int64_t EncoderWorker::maximumNvencCallLatency100ns() const {
    return maximumNvencCallLatency100ns_.load(std::memory_order_relaxed);
}

int64_t EncoderWorker::averageOutputDrainLatency100ns() const {
    return totalOutputDrainLatency100ns_.load(std::memory_order_relaxed);
}

int64_t EncoderWorker::maximumOutputDrainLatency100ns() const {
    return maximumOutputDrainLatency100ns_.load(std::memory_order_relaxed);
}

EncoderRecentPerformance EncoderWorker::recentPerformance() const {
    const int64_t now100ns = monotonicNow100ns();
    EncoderRecentPerformance result;
    result.schedulerWakeLateness = recentSchedulerWakeLateness_.snapshot(now100ns);
    result.queueResidence = recentQueueResidenceLatency_.snapshot(now100ns);
    result.inputPtsInterval = recentInputPtsInterval_.snapshot(now100ns);
    result.outputPtsInterval = recentOutputPtsInterval_.snapshot(now100ns);
    const auto distinctSourceEvents = recentDistinctSourceOutputEvents_.recentCount(now100ns);
    const auto repeatedSourceEvents = recentRepeatedSourceOutputEvents_.recentCount(now100ns);
    const auto unknownSourceEvents = recentUnknownSourceOutputEvents_.recentCount(now100ns);
    const auto motionOutputEvents = recentMotionOutputEvents_.recentCount(now100ns);
    const auto motionRepeatEvents = recentMotionRepeatEvents_.recentCount(now100ns);
    result.inputPreparation = recentInputPreparationLatency_.snapshot(now100ns);
    result.inputMap = recentInputMapLatency_.snapshot(now100ns);
    result.encodeCall = recentNvencCallLatency_.snapshot(now100ns);
    result.outputEventWait = recentOutputEventWaitLatency_.snapshot(now100ns);
    result.outputLock = recentOutputLockLatency_.snapshot(now100ns);
    result.outputCopy = recentOutputCopyLatency_.snapshot(now100ns);
    result.outputUnmap = recentOutputUnmapLatency_.snapshot(now100ns);
    const int64_t started100ns = performanceStarted100ns_.load(std::memory_order_relaxed);
    const int64_t observationWindow100ns = started100ns > 0
        ? std::min<int64_t>(now100ns - started100ns, 10'000'000LL)
        : 0;
    if (observationWindow100ns > 0) {
        result.inputFps = static_cast<double>(result.inputPreparation.samples) * 10'000'000.0 /
            static_cast<double>(observationWindow100ns);
        result.outputFps = static_cast<double>(result.outputLock.samples) * 10'000'000.0 /
            static_cast<double>(observationWindow100ns);
        const double rawDistinctFps = static_cast<double>(distinctSourceEvents) *
            10'000'000.0 / static_cast<double>(observationWindow100ns);

        // Fast Attack, Smooth Decay: Instant snap-back on motion, gentle curve on entering smoke
        if (smoothedDistinctFps_ <= 0.0 || rawDistinctFps >= smoothedDistinctFps_) {
            smoothedDistinctFps_ = rawDistinctFps;
        } else {
            const double deltaSeconds = lastSmoothedFpsTime100ns_ > 0
                ? static_cast<double>(now100ns - lastSmoothedFpsTime100ns_) / 10'000'000.0
                : 0.05;
            const double alpha = 1.0 - std::exp(-std::clamp(deltaSeconds, 0.001, 0.5) / 0.7);
            smoothedDistinctFps_ += alpha * (rawDistinctFps - smoothedDistinctFps_);
        }
        lastSmoothedFpsTime100ns_ = now100ns;
        result.distinctSourceOutputFps = smoothedDistinctFps_;
    }
    result.repeatedSourceOutputFrames = repeatedSourceEvents;
    result.unknownSourceOutputFrames = unknownSourceEvents;
    result.motionFramesTotal = motionOutputEvents;
    result.motionFramesRepeated = motionRepeatEvents;
    return result;
}

int EncoderWorker::sourceWidth() const {
    return sourceWidth_;
}

int EncoderWorker::sourceHeight() const {
    return sourceHeight_;
}

int EncoderWorker::outputWidth() const {
    return outputWidth_;
}

int EncoderWorker::outputHeight() const {
    return outputHeight_;
}

bool EncoderWorker::scalingActive() const {
    return scalingActive_;
}

std::string EncoderWorker::status() const {
    std::lock_guard lock(statusMutex_);
    return status_;
}

void EncoderWorker::setStatus(std::string status) {
    std::lock_guard lock(statusMutex_);
    status_ = std::move(status);
}

bool EncoderWorker::queueFrame(EncodeJob job) {
    std::unique_lock lock(submitMutex_);
    if (!running_.load(std::memory_order_relaxed)) return false;

    while (pendingJobs_.size() >= maximumPendingFrames_) {
        pendingJobs_.pop_front();
        pendingOutputFrames_ -= std::min<std::size_t>(pendingOutputFrames_, 1);
        pendingFreshFrames_ -= std::min<std::size_t>(pendingFreshFrames_, 1);
        ++encoderQueueDrops_;
        ++encoderBackpressureDrops_;
        ++encoderAdmissionRejections_;
    }

    job.enqueuedAt100ns = monotonicNow100ns();
    pendingJobs_.push_back(std::move(job));
    ++pendingOutputFrames_;
    ++pendingFreshFrames_;
    lock.unlock();
    ++framesAccepted_;
    submitCv_.notify_one();
    return true;
}

