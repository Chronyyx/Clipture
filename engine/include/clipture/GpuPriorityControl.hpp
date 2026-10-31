#pragma once
#include "clipture/Diagnostics.hpp"
#include "clipture/GpuPriorityMemory.hpp"
#include "clipture/GpuStarvationDetector.hpp"

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
class GpuPriorityControl {
public:
    GpuPriorityControl();
    ~GpuPriorityControl();

    // Applies the setting (CLIPTURE_GPU_SCHEDULING_CLASS overrides it for
    // engineering A/B), opens the per-game memory and records the outcome.
    void apply(bool boost, const std::string& memoryPath, Diagnostics& diagnostics);
    // One detection pass: the game in front (empty when none), every running
    // game and the engine's total dropped video frames. Any thread.
    void observe(const std::string& foregroundGame,
                 const std::vector<std::string>& runningGames,
                 uint64_t droppedTotal,
                 int64_t now100ns,
                 int fps);
    // Copies the current class into diagnostics.
    void publish(Diagnostics& diagnostics);
    // Refreshes this process's video-memory budget and usage, and the
    // adapter's usage by every process.
    void refreshMemory(Diagnostics& diagnostics);

    // Passes without the raised game before stepping down (one may miss it).
    static constexpr int kGoneGamePasses = 2;
    // Packets in flight are averaged over 25 reads 4 ms apart per pass.
    static constexpr int kInFlightSamples = 25;
    static constexpr int kInFlightIntervalMs = 4;

private:
    void raiseLocked(const std::string& game, const char* reason);
    void lowerLocked(const char* reason);
    void setClassLocked(int requested, const char* reason);

    std::mutex mutex_;
    std::optional<bool> appliedBoost_;
    bool boost_ = false;
    bool override_ = false;
    bool startRaised_ = false;
    bool raised_ = false;
    std::string raisedGame_;
    std::string raisedReason_;
    int raisedGameMissing_ = 0;
    // The game whose session was lowered for a quiet GPU: not raised again
    // from memory until it restarts.
    std::string quietGame_;
    std::string trackedGame_;
    GpuStarvationDetector starvation_;
    GpuQuietWatch quiet_;
    GpuRememberWatch remember_;
    std::optional<double> lastBusy_;
    std::optional<double> lastInFlight_;
    GpuPriorityMemory memory_;
    std::string memoryPath_;
    std::string gpuIdentity_;
    std::string requested_ = "default";
    std::string active_ = "default";
    int64_t setStatus_ = 0;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    std::unique_ptr<platform::AdapterMemoryCounter> adapterMemory_;
    std::unique_ptr<platform::AdapterBusyCounter> adapterBusy_;
    bool factsRecorded_ = false;
};

}  // namespace clipture
