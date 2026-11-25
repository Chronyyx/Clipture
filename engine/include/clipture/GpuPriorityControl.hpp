#pragma once
#include "clipture/Diagnostics.hpp"
#include "clipture/GpuPriorityMemory.hpp"
#include "clipture/GpuRaisePolicy.hpp"

#include <wrl/client.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct IDXGIAdapter1;

namespace clipture {
namespace platform {
class AdapterBusyCounter;
class AdapterMemoryCounter;
class PacketsInFlightSampler;
class ProcessGpuTime;
}

// Owns the engine's GPU scheduling class (ADR 0025, ADR 0029). With the boost
// on, the engine runs at normal and raises its work (capture copies,
// conversion, NVENC) to realtime for the game in front when it starves
// recording (GpuStarvationDetector), or at once for a game remembered on this
// GPU (GpuRememberWatch). It returns to normal when the game closes, or when so little work
// stays queued on the GPU that no game can starve it (fps capped); it never
// tries normal just to see (that would put stutter in clips). Realtime, not high: high measured
// worse than normal with hardware scheduling on. The host turns the boost off
// after an encoder hang.
//
// A raise takes effect only once the encoder has run about two seconds since
// its last setup, and not while adapter memory is nearly full and still
// moving: NVENC and surface allocation at realtime on a full card can hang the
// driver. An encoder setup while raised drops to normal until it has run again.
class GpuPriorityControl {
public:
    // The encoder's progress, read by the detection pass.
    struct PipelineProgress {
        uint64_t encoderSetups = 0;
        uint64_t framesEncoded = 0;
    };

    GpuPriorityControl();
    ~GpuPriorityControl();

    // Applies the setting (CLIPTURE_GPU_SCHEDULING_CLASS overrides it for
    // engineering A/B), opens the per-game memory and records the outcome.
    void apply(bool boost, const std::string& memoryPath, Diagnostics& diagnostics);
    // One detection pass: the game in front (empty when none), every running
    // game and the engine's total dropped video frames. Any thread.
    // `foregroundApp` is the app in front when no known game is (else empty).
    void observe(const std::string& foregroundGame,
                 const std::string& foregroundApp,
                 const std::vector<std::string>& runningGames,
                 uint64_t droppedTotal,
                 int64_t now100ns,
                 int fps,
                 PipelineProgress pipeline = {});
    // The encoder is about to set up a session (any thread): drop to normal
    // until it has run again.
    void encoderSetupStarting();
    // Copies the current class into diagnostics.
    void publish(Diagnostics& diagnostics);
    // The class in effect, as diagnostics name it.
    std::string schedulingClass();
    // Refreshes this process's video-memory budget and usage and the adapter's
    // usage by every process. With `sampleBusy`, also how busy the adapter was
    // since the previous sampled call; nothing in the engine acts on that
    // figure, so it is read only for a caller that shows it. Called from one
    // thread (the request loop).
    void refreshUsage(Diagnostics& diagnostics, bool sampleBusy);

    // A busy figure this old is reported as unknown, and a busy counter left
    // unread this long is primed again instead of averaging the whole gap.
    static constexpr int64_t kBusyMaximumAge100ns = 10LL * 10'000'000LL;

    // Packets in flight are averaged over 25 reads 4 ms apart per pass.
    static constexpr int kInFlightSamples = 25;
    static constexpr int kInFlightIntervalMs = 4;
    // Encoded seconds after a setup before realtime may apply.
    static constexpr int kSetupSettleSeconds = 2;
    // Adapter memory counts as nearly full within this much of its size, and
    // as moving when it changed this much since the previous pass.
    static constexpr uint64_t kVramHeadroomBytes = 1536ull * 1024ull * 1024ull;
    static constexpr uint64_t kVramMovingBytes = 256ull * 1024ull * 1024ull;
    // Passes a raise waits on moving memory before applying anyway.
    static constexpr int kMaxVramWaitPasses = 10;

private:
    void setClassLocked(int requested, const char* reason);
    // Applies realtime for a raise unless a setup or moving memory holds it.
    void applyRaiseLocked();
    void updateGatesLocked(PipelineProgress pipeline, int fps, std::optional<uint64_t> vramUsage);
    // Whether this pass acts on packets in flight: only for a game in front
    // that could be raised, or one running at realtime. Reading them costs
    // milliseconds of CPU a pass, so other passes skip it.
    bool inFlightNeeded(const std::string& foregroundGame, uint64_t droppedTotal);

    std::mutex mutex_;
    std::optional<bool> appliedBoost_;
    bool boost_ = false;
    bool override_ = false;
    bool startRaised_ = false;
    bool realtimeApplied_ = false;
    // CLIPTURE_GPU_PRIORITY_START_RAISED sessions skip the gates (smoke tests).
    bool gateBypass_ = false;
    bool setupInProgress_ = false;
    bool pipelineReady_ = false;
    uint64_t seenSetups_ = 0;
    uint64_t setupFramesBase_ = 0;
    bool vramMoving_ = false;
    int vramWaitPasses_ = 0;
    std::optional<uint64_t> lastVramUsage_;
    uint64_t vramTotal_ = 0;
    std::string holdReason_;
    // Why the scheduling class last changed (policy decision or gate), and
    // the known game the last detection pass saw in front (empty when none).
    std::string changeReason_;
    std::string policyForeground_;
    GpuRaisePolicy policy_;
    // Touched only by refreshUsage().
    std::optional<double> lastBusy_;
    int64_t busySampledAt100ns_ = 0;
    std::optional<double> lastInFlight_;
    GpuPriorityMemory memory_;
    std::string memoryPath_;
    std::string gpuIdentity_;
    std::string requested_ = "default";
    std::string active_ = "default";
    int64_t setStatus_ = 0;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    std::unique_ptr<platform::AdapterMemoryCounter> adapterMemory_;
    // Read by the detection pass; adapterMemory_ serves diagnostics.
    std::unique_ptr<platform::AdapterMemoryCounter> gateMemory_;
    std::unique_ptr<platform::AdapterBusyCounter> adapterBusy_;
    // The foreground process's own GPU share: tells a game still drawing
    // from a static screen when capture sees no new frames.
    std::unique_ptr<platform::ProcessGpuTime> foregroundGpu_;
    // Used by the detection pass only.
    std::unique_ptr<platform::PacketsInFlightSampler> inFlightSampler_;
    bool factsRecorded_ = false;
};

}  // namespace clipture
