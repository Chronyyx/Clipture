#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct IDXGIAdapter1;

namespace clipture::platform {

// What the driver reports about the adapter the engine encodes on. Recorded
// in diagnostics so a capture-stall report carries the scheduling context.
struct GpuAdapterFacts {
    std::string driverVersion = "unknown";
    // Hardware-accelerated GPU scheduling: "on", "off" or "unknown".
    std::string hardwareScheduling = "unknown";
    uint64_t dedicatedVideoMemoryBytes = 0;
};

// The OS's local video-memory budget for this process and what it uses now.
struct VideoMemoryUsage {
    bool available = false;
    uint64_t budgetBytes = 0;
    uint64_t processUsageBytes = 0;
};

GpuAdapterFacts queryGpuAdapterFacts(IDXGIAdapter1* adapter);
VideoMemoryUsage queryVideoMemoryUsage(IDXGIAdapter1* adapter);

// Dedicated video memory in use on the adapter by every process (the game
// included), from the "GPU Adapter Memory" performance counter.
class AdapterMemoryCounter {
public:
    explicit AdapterMemoryCounter(IDXGIAdapter1* adapter);
    ~AdapterMemoryCounter();
    AdapterMemoryCounter(const AdapterMemoryCounter&) = delete;
    AdapterMemoryCounter& operator=(const AdapterMemoryCounter&) = delete;

    std::optional<uint64_t> dedicatedUsageBytes();

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace clipture::platform
