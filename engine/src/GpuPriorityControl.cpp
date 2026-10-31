#include "clipture/GpuPriorityControl.hpp"

#include "clipture/NativeRuntime.hpp"
#include "clipture/platform/windows/GpuAdapterFacts.hpp"
#include "clipture/platform/windows/GpuSchedulingPriority.hpp"

#include <Windows.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <iostream>
#include <sstream>

namespace clipture {
namespace {

constexpr uint64_t kMegabyte = 1024ull * 1024ull;

// CLIPTURE_GPU_SCHEDULING_CLASS (engineering A/B) replaces the setting.
std::optional<platform::GpuSchedulingClass> environmentOverride() {
    wchar_t value[32] {};
    const DWORD count = GetEnvironmentVariableW(L"CLIPTURE_GPU_SCHEDULING_CLASS", value, 32);
    if (count == 0 || count >= 32) return std::nullopt;
    platform::GpuSchedulingClass parsed {};
    if (!platform::parseGpuSchedulingClass(value, parsed)) {
        std::cerr << "[engine] GPU scheduling class override ignored: unknown value.\n";
        return std::nullopt;
    }
    return parsed;
}

// CLIPTURE_GPU_PRIORITY_START_RAISED=1 (smoke tests) starts at realtime as if
// starvation had already been seen; the boost setting still gates it.
bool startRaised() {
    wchar_t value[4] {};
    const DWORD count = GetEnvironmentVariableW(L"CLIPTURE_GPU_PRIORITY_START_RAISED", value, 4);
    return count == 1 && value[0] == L'1';
}

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return text;
}

// The GPU's hardware IDs: the per-game memory is per GPU.
std::string adapterIdentity(IDXGIAdapter1* adapter) {
    DXGI_ADAPTER_DESC1 desc {};
    if (!adapter || FAILED(adapter->GetDesc1(&desc))) return "unknown";
    std::ostringstream identity;
    identity << std::hex << desc.VendorId << ':' << desc.DeviceId << ':' << desc.SubSysId << ':' << desc.Revision;
    return identity.str();
}

}  // namespace

GpuPriorityControl::GpuPriorityControl() : startRaised_(startRaised()) {
    std::string ignoredName;
    adapter_ = findPreferredAdapter(ignoredName);
    adapterMemory_ = std::make_unique<platform::AdapterMemoryCounter>(adapter_.Get());
    adapterBusy_ = std::make_unique<platform::AdapterBusyCounter>(adapter_.Get());
    gpuIdentity_ = adapterIdentity(adapter_.Get());
}

GpuPriorityControl::~GpuPriorityControl() = default;

void GpuPriorityControl::apply(bool boost, const std::string& memoryPath, Diagnostics& diagnostics) {
    if (!factsRecorded_) {
        const auto facts = platform::queryGpuAdapterFacts(adapter_.Get());
        diagnostics.gpuDriverVersion = facts.driverVersion;
        diagnostics.hardwareGpuScheduling = facts.hardwareScheduling;
        diagnostics.vramDedicatedMb = facts.dedicatedVideoMemoryBytes / kMegabyte;
        factsRecorded_ = true;
        std::cerr << "[engine] GPU hags=" << facts.hardwareScheduling
                  << " driver=" << facts.driverVersion << ".\n";
    }
    {
        std::lock_guard lock(mutex_);
        if (memoryPath != memoryPath_) {
            memoryPath_ = memoryPath;
            memory_.open(memoryPath, gpuIdentity_);
        }
        if (appliedBoost_ != boost) {
            appliedBoost_ = boost;
            boost_ = boost;
            starvation_.reset();
            trackedGame_.clear();
            const auto override = environmentOverride();
            override_ = override.has_value();
            if (override) {
                raised_ = false;
                setClassLocked(static_cast<int>(*override), "override");
            } else if (boost && startRaised_) {
                startRaised_ = false;
                raiseLocked({}, "start raised");
            } else if (!boost || !raised_) {
                // Turning the boost off (by hand or after a hang) drops a raise.
                raised_ = false;
                raisedGame_.clear();
                raisedReason_.clear();
                setClassLocked(static_cast<int>(platform::GpuSchedulingClass::Normal), "configure");
            }
        }
    }
    publish(diagnostics);
}

void GpuPriorityControl::observe(const std::string& foregroundGame,
                                 const std::vector<std::string>& runningGames,
                                 uint64_t droppedTotal,
                                 int64_t now100ns,
                                 int fps) {
    // Sampled before taking the lock: it sleeps about 100 ms, and publish()
    // must not wait on it.
    const auto inFlight = platform::meanPacketsInFlight(adapter_.Get(), kInFlightSamples, kInFlightIntervalMs);
    const auto busy = adapterBusy_->busyPercent();
    std::lock_guard lock(mutex_);
    lastBusy_ = busy;
    lastInFlight_ = inFlight;
    if (!boost_ || override_) return;
    std::vector<std::string> running;
    running.reserve(runningGames.size());
    for (const auto& game : runningGames) running.push_back(lowerAscii(game));
    const auto foreground = lowerAscii(foregroundGame);

    if (raised_) {
        if (raisedGame_.empty()) return;
        if (std::find(running.begin(), running.end(), raisedGame_) != running.end()) {
            raisedGameMissing_ = 0;
        } else if (++raisedGameMissing_ >= kGoneGamePasses) {
            lowerLocked("game closed");
            return;
        }
        // Remembered once the queue stays deep for two minutes this session.
        if (raisedReason_ != "remembered" && remember_.observe(lastInFlight_, now100ns) && memory_.enabled()) {
            if (memory_.remember(raisedGame_)) {
                std::cerr << "[engine] GPU priority: game remembered for realtime.\n";
            } else {
                std::cerr << "[engine] GPU priority memory could not be saved.\n";
            }
        }
        // One quiet step-down per game session: if drops raised this game
        // again after one, realtime itself shortened its queue, and stepping
        // down again would cycle stutter into clips until the game closes.
        if (raisedGame_ != quietGame_ && quiet_.observe(lastInFlight_, now100ns)) {
            quietGame_ = raisedGame_;
            lowerLocked("gpu quiet");
        }
        return;
    }

    // A game that starved recording on this GPU before starts raised, unless
    // this session of it was already lowered for a quiet GPU.
    if (!quietGame_.empty() && std::find(running.begin(), running.end(), quietGame_) == running.end()) {
        quietGame_.clear();
    }
    if (!foreground.empty() && foreground != quietGame_ && memory_.contains(foreground)) {
        raiseLocked(foreground, "remembered");
        return;
    }
    for (const auto& game : running) {
        if (game != quietGame_ && memory_.contains(game)) {
            raiseLocked(game, "remembered");
            return;
        }
    }

    // Starvation is judged per game in front; drops on the desktop or after a
    // switch to another game start the history over.
    if (foreground != trackedGame_) {
        trackedGame_ = foreground;
        starvation_.reset();
    }
    if (foreground.empty()) return;
    const auto verdict = starvation_.observe(droppedTotal, lastInFlight_, now100ns, fps);
    if (verdict == GpuStarvationDetector::Verdict::None) return;
    raiseLocked(foreground, verdict == GpuStarvationDetector::Verdict::QueuedDrops ? "queued drops" : "steady drops");
}

void GpuPriorityControl::publish(Diagnostics& diagnostics) {
    std::lock_guard lock(mutex_);
    diagnostics.gpuPriorityBoost = boost_;
    diagnostics.gpuPriorityRaised = raised_;
    diagnostics.gpuPriorityReason = raisedReason_;
    diagnostics.gpuPriorityRememberedGames = static_cast<int>(memory_.size());
    diagnostics.gpuStarvationLossyWindows = starvation_.lossyWindows();
    diagnostics.gpuBusyPercent = lastBusy_.value_or(-1.0);
    diagnostics.gpuPacketsInFlight = lastInFlight_.value_or(-1.0);
    diagnostics.gpuSchedulingRequested = requested_;
    diagnostics.gpuSchedulingOverride = override_;
    diagnostics.gpuSchedulingClass = active_;
    diagnostics.gpuSchedulingStatus = setStatus_;
}

void GpuPriorityControl::raiseLocked(const std::string& game, const char* reason) {
    raised_ = true;
    raisedGame_ = game;
    raisedReason_ = reason;
    raisedGameMissing_ = 0;
    quiet_.reset();
    remember_.reset();
    setClassLocked(static_cast<int>(platform::GpuSchedulingClass::Realtime), reason);
}

void GpuPriorityControl::lowerLocked(const char* reason) {
    raised_ = false;
    raisedGame_.clear();
    raisedReason_.clear();
    trackedGame_.clear();
    starvation_.reset();
    setClassLocked(static_cast<int>(platform::GpuSchedulingClass::Normal), reason);
}

void GpuPriorityControl::setClassLocked(int requested, const char* reason) {
    const auto result = platform::setProcessGpuSchedulingClass(
        GetCurrentProcess(), static_cast<platform::GpuSchedulingClass>(requested));
    requested_ = platform::gpuSchedulingClassName(requested);
    active_ = result.available ? platform::gpuSchedulingClassName(result.active) : "unavailable";
    setStatus_ = static_cast<int64_t>(result.setStatus);
    std::cerr << "[engine] GPU scheduling class requested=" << requested_
              << " (" << reason << ")"
              << " active=" << active_
              << " status=0x" << std::hex << static_cast<unsigned long>(result.setStatus) << std::dec
              << ".\n";
}

void GpuPriorityControl::refreshMemory(Diagnostics& diagnostics) {
    const auto usage = platform::queryVideoMemoryUsage(adapter_.Get());
    if (usage.available) {
        diagnostics.vramBudgetMb = usage.budgetBytes / kMegabyte;
        diagnostics.vramProcessUsageMb = usage.processUsageBytes / kMegabyte;
    }
    if (const auto adapterUsage = adapterMemory_->dedicatedUsageBytes()) {
        diagnostics.vramAdapterUsageMb = *adapterUsage / kMegabyte;
    }
}

}  // namespace clipture
