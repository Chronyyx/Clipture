#pragma once
#include "clipture/Diagnostics.hpp"

#include <wrl/client.h>

#include <memory>
#include <optional>

struct IDXGIAdapter1;

namespace clipture {
namespace platform { class AdapterMemoryCounter; }

// Owns the engine's GPU scheduling class. With the boost on, the engine's GPU
// work (capture copies, conversion, NVENC) is scheduled ahead of a game that
// saturates the GPU; without it, an uncapped game starves the encoder and
// capture freezes. Realtime, not high: high measured worse than normal with
// hardware scheduling on. The host turns the boost off after an encoder hang.
class GpuPriorityControl {
public:
    GpuPriorityControl();
    ~GpuPriorityControl();

    // Applies the class for the setting (CLIPTURE_GPU_SCHEDULING_CLASS
    // overrides it for engineering A/B) and records the outcome.
    void apply(bool boost, Diagnostics& diagnostics);
    // Refreshes this process's video-memory budget and usage, and the
    // adapter's usage by every process.
    void refreshMemory(Diagnostics& diagnostics);

private:
    std::optional<bool> appliedBoost_;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    std::unique_ptr<platform::AdapterMemoryCounter> adapterMemory_;
    bool factsRecorded_ = false;
};

}  // namespace clipture
