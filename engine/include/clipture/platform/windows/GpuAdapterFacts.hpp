#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
//
// Reading one engine node's statistics costs about 0.3 ms of CPU and an
// adapter has a dozen nodes, most of them unused. Every node is read on a
// pass's first sample; one that submitted nothing since the previous pass and
// holds nothing in flight is not read again for the rest of the pass.
class PacketsInFlightSampler {
public:
    explicit PacketsInFlightSampler(IDXGIAdapter1* adapter);
    ~PacketsInFlightSampler();
    PacketsInFlightSampler(const PacketsInFlightSampler&) = delete;
    PacketsInFlightSampler& operator=(const PacketsInFlightSampler&) = delete;

    std::optional<double> mean(int samples, int intervalMs);
    // Node reads the last mean() made; for tests.
    int lastNodeReads() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

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

// How busy one process keeps the GPU: the busiest engine's running time for
// that process since the previous read, as a percentage of the time between
// the reads. Per process, so another program's load (a local LLM, a video)
// never counts toward it. Reads only when asked (one kernel call per engine),
// keeps no thread, never sleeps. The first read of a process, or a process
// the caller may not query, gives nothing.
class ProcessGpuTime {
public:
    explicit ProcessGpuTime(IDXGIAdapter1* adapter);
    ~ProcessGpuTime();
    ProcessGpuTime(const ProcessGpuTime&) = delete;
    ProcessGpuTime& operator=(const ProcessGpuTime&) = delete;

    std::optional<double> percent(unsigned long processId, int64_t now100ns);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// The pure part of ProcessGpuTime: per-engine running times (100 ns) at two
// reads `elapsed100ns` apart, to the busiest engine's share in percent.
std::optional<double> busiestEngineSharePercent(
    const std::vector<int64_t>& before, const std::vector<int64_t>& after, int64_t elapsed100ns);

}  // namespace clipture::platform
