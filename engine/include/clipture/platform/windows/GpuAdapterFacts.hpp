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

// Work packets submitted to the adapter's engines and not yet completed, from
// every process, averaged over `samples` reads `intervalMs` apart (the call
// sleeps for that long). A game that queues frames ahead of the GPU keeps
// dozens in flight; one that is capped or waits on its last frame keeps a
// handful (ADR 0029). Unlike adapter busy, it does not under-read with
// hardware scheduling on. nullopt when the statistics are unavailable.
std::optional<double> meanPacketsInFlight(IDXGIAdapter1* adapter, int samples, int intervalMs);

// How busy the adapter's 3D engines are, summed over every process (the
// game's share is most of it), from the counters Task Manager shows. Reads the
// same at any scheduling class, so it still describes the game's load while
// the engine is raised (ADR 0029). The first read only primes the counter.
class AdapterBusyCounter {
public:
    explicit AdapterBusyCounter(IDXGIAdapter1* adapter);
    ~AdapterBusyCounter();
    AdapterBusyCounter(const AdapterBusyCounter&) = delete;
    AdapterBusyCounter& operator=(const AdapterBusyCounter&) = delete;

    // Percent 0-100 since the previous read; nullopt when unknown.
    std::optional<double> busyPercent();

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace clipture::platform
