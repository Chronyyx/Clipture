#pragma once
#include "clipture/FrameSelectionTrace.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace clipture {

class DeferredFramePreparation;
class GpuTextureReadState;

struct CapturedFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    std::shared_ptr<void> textureLease;
    int64_t pts100ns = 0;
    int width = 0;
    int height = 0;
    int64_t queuedAtSteady100ns = 0;
    uint64_t captureEpoch = 0;
    uint64_t sequence = 0;
    bool sourceHadDesktopPresent = false;
    bool sourceHadPointerUpdate = false;
    std::shared_ptr<DeferredFramePreparation> preparation;
    std::shared_ptr<GpuTextureReadState> gpuReadState;
};

struct FrameQueueStats {
    uint64_t pushedFrames = 0;
    uint64_t overflowDrops = 0;
    uint64_t coalescedDrops = 0;
    uint64_t clearedFrames = 0;
    std::size_t maximumDepth = 0;
};

class FrameQueue {
public:
    explicit FrameQueue(std::size_t capacity = 8);

    void push(CapturedFrame frame);
    std::optional<CapturedFrame> waitPop();
    std::optional<CapturedFrame> waitPopFor(std::chrono::milliseconds timeout);
    std::optional<CapturedFrame> waitConsumeLatestUntil(
        std::chrono::steady_clock::time_point deadline);
    std::optional<CapturedFrame> consumeAllAndGetLatest(int64_t deadline100ns = 0, int fps = 0);
    std::optional<CapturedFrame> consumeLatestAtOrBefore(int64_t pts100ns);
    void stop();
    void clear();

    std::size_t size() const;
    int droppedFrames() const;
    int64_t oldestFrameAge100ns() const;
    FrameQueueStats stats() const;

    // The encoder cadence announces its next selection deadline (monotonic
    // 100 ns; 0 when unknown) so capture can skip desktop updates that a newer
    // update would supersede before that tick.
    void announceNextDeadline(int64_t deadline100ns) {
        nextDeadline100ns_.store(deadline100ns, std::memory_order_relaxed);
    }
    int64_t nextDeadline100ns() const { return nextDeadline100ns_.load(std::memory_order_relaxed); }

    // The encoder's remaining selection ticks, in frame (media) time: the next
    // unconsumed tick and the rate of the ones after it; 0 clears. A queued
    // frame that a newer one replaces before any of these ticks can never be
    // selected, so push() releases it (and its capture slot) at once instead
    // of at the next tick. The encoded output is unchanged.
    void announceSelectionTicks(int64_t nextTickPts100ns, int fps);

    // push()'s release rule for frames not yet queued: true when no remaining
    // tick could select a frame at `olderPts` once one at `newerPts` exists.
    bool wouldSupersede(int64_t olderPts100ns, int64_t newerPts100ns) const;

    // Capture may hold its newest frame unprepared (HeldFrame.hpp). The encoder
    // calls beforeSelect() when it wakes for a tick, before announcing the next
    // one, so that frame is queued for the tick that may select it.
    void setBeforeSelect(std::function<void()> hook);
    void beforeSelect();

    // Selection trace only (CLIPTURE_SELECTION_TRACE_PATH): capture-side
    // events, `source` in system-relative 100 ns.
    void traceCapture(char kind, int64_t source100ns, uint32_t flags, uint64_t sequence = 0);

private:
    bool supersededBy(int64_t olderPts100ns, int64_t newerPts100ns) const;

    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<CapturedFrame> frames_;
    bool stopped_ = false;
    FrameQueueStats stats_;
    FrameSelectionTrace selectionTrace_;
    std::atomic<int64_t> nextDeadline100ns_ = 0;
    int64_t nextTickPts100ns_ = 0;
    int tickFps_ = 0;
    std::mutex hookMutex_;
    std::function<void()> beforeSelect_;
};

}  // namespace clipture
