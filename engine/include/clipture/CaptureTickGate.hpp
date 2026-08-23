#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>

namespace clipture {

// Continuous decoupled DXGI sampler captures DWM flips at high refresh rate (144Hz/240Hz)
// as they occur without scheduler-to-capture thread hop jitter, while EncoderWorker consumes the newest frame.
inline constexpr bool kEnableEncoderDrivenDxgiCapture = false;
inline constexpr uint32_t kEncoderDrivenDxgiPollTimeoutMs = 0;
inline constexpr bool kEnableEncoderDrivenDxgiGrace = false;
inline constexpr uint32_t kEncoderDrivenDxgiGraceTimeoutMs = 0;
inline constexpr bool kEnableEncoderDrivenDxgiPrearm = false;
inline constexpr auto kEncoderDrivenCapturePrearmLead =
    std::chrono::milliseconds(0);
inline constexpr auto kEncoderDrivenCaptureCompletionWait =
    std::chrono::milliseconds(10);

inline constexpr bool shouldUseEncoderDrivenDxgiGrace(
    bool encoderDriven,
    bool bootstrapComplete,
    bool initialPollTimedOut) {
    return kEnableEncoderDrivenDxgiGrace && encoderDriven &&
        bootstrapComplete && initialPollTimedOut;
}

struct CaptureTickGateStats {
    bool active = false;
    uint64_t requests = 0;
    uint64_t wakeups = 0;
    uint64_t coalescedRequests = 0;
    uint64_t completions = 0;
    uint64_t completionWaits = 0;
    uint64_t completionWaitTimeouts = 0;
};

class CaptureTickGate {
public:
    void reset() {
        {
            std::lock_guard lock(mutex_);
            active_.store(false, std::memory_order_release);
            requestedGeneration_ = 0;
            consumedGeneration_ = 0;
            completedGeneration_ = 0;
            requests_ = 0;
            wakeups_ = 0;
            coalescedRequests_ = 0;
            completions_ = 0;
            completionWaits_ = 0;
            completionWaitTimeouts_ = 0;
        }
        wake_.notify_all();
    }

    void activate() {
        {
            std::lock_guard lock(mutex_);
            requestedGeneration_ = 0;
            consumedGeneration_ = 0;
            completedGeneration_ = 0;
            active_.store(true, std::memory_order_release);
        }
        wake_.notify_all();
    }

    void deactivate() {
        active_.store(false, std::memory_order_release);
        wake_.notify_all();
    }

    bool active() const {
        return active_.load(std::memory_order_acquire);
    }

    std::optional<uint64_t> request() {
        if (!active()) return std::nullopt;
        uint64_t generation = 0;
        {
            std::lock_guard lock(mutex_);
            if (!active_.load(std::memory_order_relaxed)) return std::nullopt;
            generation = ++requestedGeneration_;
            ++requests_;
        }
        wake_.notify_one();
        return generation;
    }

    std::optional<uint64_t> wait(std::stop_token stopToken) {
        std::unique_lock lock(mutex_);
        const bool ready = wake_.wait(lock, stopToken, [this] {
            return !active_.load(std::memory_order_relaxed) ||
                requestedGeneration_ > consumedGeneration_;
        });
        if (!ready || stopToken.stop_requested() ||
            !active_.load(std::memory_order_relaxed)) {
            return std::nullopt;
        }

        const uint64_t pending = requestedGeneration_ - consumedGeneration_;
        if (pending > 1) coalescedRequests_ += pending - 1;
        consumedGeneration_ = requestedGeneration_;
        ++wakeups_;
        return consumedGeneration_;
    }

    void complete(uint64_t generation) {
        if (generation == 0) return;
        {
            std::lock_guard lock(mutex_);
            if (generation <= completedGeneration_) return;
            completedGeneration_ = generation;
            ++completions_;
        }
        wake_.notify_all();
    }

    bool waitForCompletion(
        uint64_t generation,
        std::chrono::milliseconds timeout = kEncoderDrivenCaptureCompletionWait) {
        if (generation == 0) return false;
        std::unique_lock lock(mutex_);
        if (!active_.load(std::memory_order_relaxed)) return false;
        ++completionWaits_;
        const bool ready = wake_.wait_for(lock, timeout, [this, generation] {
            return !active_.load(std::memory_order_relaxed) ||
                completedGeneration_ >= generation;
        });
        if (!ready) {
            ++completionWaitTimeouts_;
            return false;
        }
        return completedGeneration_ >= generation;
    }

    CaptureTickGateStats stats() const {
        std::lock_guard lock(mutex_);
        return CaptureTickGateStats {
            active_.load(std::memory_order_relaxed),
            requests_,
            wakeups_,
            coalescedRequests_,
            completions_,
            completionWaits_,
            completionWaitTimeouts_,
        };
    }

private:
    std::atomic<bool> active_ = false;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    uint64_t requestedGeneration_ = 0;
    uint64_t consumedGeneration_ = 0;
    uint64_t completedGeneration_ = 0;
    uint64_t requests_ = 0;
    uint64_t wakeups_ = 0;
    uint64_t coalescedRequests_ = 0;
    uint64_t completions_ = 0;
    uint64_t completionWaits_ = 0;
    uint64_t completionWaitTimeouts_ = 0;
};

}  // namespace clipture
