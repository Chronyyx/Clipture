#include "clipture/GpuPriorityControl.hpp"

#include "clipture/MediaClock.hpp"
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

GpuPriorityControl::GpuPriorityControl() : startRaised_(startRaised()), gateBypass_(startRaised_) {
    std::string ignoredName;
    adapter_ = findPreferredAdapter(ignoredName);
    adapterMemory_ = std::make_unique<platform::AdapterMemoryCounter>(adapter_.Get());
    gateMemory_ = std::make_unique<platform::AdapterMemoryCounter>(adapter_.Get());
    vramTotal_ = platform::queryGpuAdapterFacts(adapter_.Get()).dedicatedVideoMemoryBytes;
    adapterBusy_ = std::make_unique<platform::AdapterBusyCounter>(adapter_.Get());
    inFlightSampler_ = std::make_unique<platform::PacketsInFlightSampler>(adapter_.Get());
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
            const auto override = environmentOverride();
            override_ = override.has_value();
            if (override) {
                policy_.reset();
                setClassLocked(static_cast<int>(*override), "override");
            } else if (boost && startRaised_) {
                startRaised_ = false;
                policy_.startRaised();
                vramWaitPasses_ = 0;
                applyRaiseLocked();
            } else {
                // Turning the boost off (by hand or after a hang) drops a raise;
                // turning it on starts at normal.
                policy_.reset();
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
                                 int fps,
                                 PipelineProgress pipeline) {
    const auto foreground = lowerAscii(foregroundGame);
    // Sampled before taking the lock: it sleeps about 100 ms, and publish()
    // must not wait on it.
    const auto inFlight = inFlightNeeded(foreground, droppedTotal)
        ? inFlightSampler_->mean(kInFlightSamples, kInFlightIntervalMs)
        : std::nullopt;
    const auto vramUsage = gateMemory_->dedicatedUsageBytes();
    std::lock_guard lock(mutex_);
    lastInFlight_ = inFlight;
    policyForeground_ = foreground;
    updateGatesLocked(pipeline, fps, vramUsage);
    if (!boost_ || override_) return;
    applyRaiseLocked();

    GpuRaisePolicy::Pass pass;
    pass.foreground = foreground;
    pass.running.reserve(runningGames.size());
    for (const auto& game : runningGames) pass.running.push_back(lowerAscii(game));
    pass.droppedTotal = droppedTotal;
    pass.inFlight = inFlight;
    pass.now100ns = now100ns;
    pass.fps = fps;
    pass.realtimeInEffect = realtimeApplied_;
    const auto decision = policy_.observe(pass, [this](const std::string& game) { return memory_.contains(game); });
    if (decision.rememberGame && memory_.enabled()) {
        if (memory_.remember(decision.game)) {
            std::cerr << "[engine] GPU priority: game remembered for realtime.\n";
        } else {
            std::cerr << "[engine] GPU priority memory could not be saved.\n";
        }
    }
    if (decision.action == GpuRaisePolicy::Action::Raise) {
        vramWaitPasses_ = 0;
        applyRaiseLocked();
    } else if (decision.action == GpuRaisePolicy::Action::Lower) {
        holdReason_.clear();
        setClassLocked(static_cast<int>(platform::GpuSchedulingClass::Normal), decision.reason);
    }
}

bool GpuPriorityControl::inFlightNeeded(const std::string& foregroundGame, uint64_t droppedTotal) {
    std::lock_guard lock(mutex_);
    if (!boost_ || override_) return false;
    return policy_.needsInFlight(foregroundGame, droppedTotal);
}

void GpuPriorityControl::publish(Diagnostics& diagnostics) {
    std::lock_guard lock(mutex_);
    diagnostics.gpuPriorityBoost = boost_;
    diagnostics.gpuPriorityRaised = policy_.raised();
    diagnostics.gpuPriorityReason = policy_.reason();
    diagnostics.gpuPriorityRememberedGames = static_cast<int>(memory_.size());
    diagnostics.gpuStarvationLossyWindows = policy_.lossyWindows();
    diagnostics.gpuPacketsInFlight = lastInFlight_.value_or(-1.0);
    diagnostics.gpuSchedulingRequested = requested_;
    diagnostics.gpuSchedulingChangeReason = changeReason_;
    diagnostics.gpuPriorityForeground = policyForeground_;
    diagnostics.gpuPriorityHold = holdReason_;
    diagnostics.gpuPolicyIneffectiveBefore = policy_.lastIneffectiveBefore();
    diagnostics.gpuPolicyIneffectiveDuring = policy_.lastIneffectiveDuring();
    diagnostics.gpuSchedulingOverride = override_;
    diagnostics.gpuSchedulingClass = active_;
    diagnostics.gpuSchedulingStatus = setStatus_;
}

void GpuPriorityControl::encoderSetupStarting() {
    std::lock_guard lock(mutex_);
    setupInProgress_ = true;
    pipelineReady_ = false;
    if (gateBypass_ || !realtimeApplied_) return;
    holdReason_ = "encoder setup";
    setClassLocked(static_cast<int>(platform::GpuSchedulingClass::Normal), "encoder setup");
}

void GpuPriorityControl::updateGatesLocked(PipelineProgress pipeline, int fps, std::optional<uint64_t> vramUsage) {
    if (pipeline.encoderSetups != seenSetups_) {
        seenSetups_ = pipeline.encoderSetups;
        setupFramesBase_ = pipeline.framesEncoded;
        setupInProgress_ = false;
        pipelineReady_ = false;
    }
    const uint64_t settleFrames = static_cast<uint64_t>(std::max(1, fps)) * kSetupSettleSeconds;
    if (!pipelineReady_ && !setupInProgress_ && seenSetups_ > 0 &&
        pipeline.framesEncoded >= setupFramesBase_ + settleFrames) {
        pipelineReady_ = true;
    }
    vramMoving_ = false;
    if (vramUsage && vramTotal_ > 0) {
        const bool nearlyFull = *vramUsage + kVramHeadroomBytes > vramTotal_;
        const uint64_t change = lastVramUsage_
            ? (*vramUsage > *lastVramUsage_ ? *vramUsage - *lastVramUsage_ : *lastVramUsage_ - *vramUsage)
            : 0;
        vramMoving_ = nearlyFull && change >= kVramMovingBytes;
    }
    lastVramUsage_ = vramUsage;
}

void GpuPriorityControl::applyRaiseLocked() {
    if (!policy_.raised() || realtimeApplied_) return;
    const char* hold = nullptr;
    if (!gateBypass_) {
        if (!pipelineReady_) {
            hold = "encoder setup";
        } else if (vramMoving_ && vramWaitPasses_ < kMaxVramWaitPasses) {
            ++vramWaitPasses_;
            hold = "video memory moving";
        }
    }
    if (hold) {
        if (holdReason_ != hold) {
            holdReason_ = hold;
            std::cerr << "[engine] GPU priority raise held at normal (" << hold << ").\n";
        }
        return;
    }
    vramWaitPasses_ = 0;
    holdReason_.clear();
    setClassLocked(static_cast<int>(platform::GpuSchedulingClass::Realtime), policy_.reason().c_str());
}

void GpuPriorityControl::setClassLocked(int requested, const char* reason) {
    const auto result = platform::setProcessGpuSchedulingClass(
        GetCurrentProcess(), static_cast<platform::GpuSchedulingClass>(requested));
    realtimeApplied_ = requested == static_cast<int>(platform::GpuSchedulingClass::Realtime);
    requested_ = platform::gpuSchedulingClassName(requested);
    changeReason_ = reason;
    active_ = result.available ? platform::gpuSchedulingClassName(result.active) : "unavailable";
    setStatus_ = static_cast<int64_t>(result.setStatus);
    std::cerr << "[engine] GPU scheduling class requested=" << requested_
              << " (" << reason << ")"
              << " active=" << active_
              << " status=0x" << std::hex << static_cast<unsigned long>(result.setStatus) << std::dec
              << ".\n";
}

std::string GpuPriorityControl::schedulingClass() {
    std::lock_guard lock(mutex_);
    return active_;
}

void GpuPriorityControl::refreshUsage(Diagnostics& diagnostics, bool sampleBusy) {
    const auto usage = platform::queryVideoMemoryUsage(adapter_.Get());
    if (usage.available) {
        diagnostics.vramBudgetMb = usage.budgetBytes / kMegabyte;
        diagnostics.vramProcessUsageMb = usage.processUsageBytes / kMegabyte;
    }
    if (const auto adapterUsage = adapterMemory_->dedicatedUsageBytes()) {
        diagnostics.vramAdapterUsageMb = *adapterUsage / kMegabyte;
    }

    const int64_t now100ns = monotonicNow100ns();
    const bool recent = busySampledAt100ns_ != 0 && now100ns - busySampledAt100ns_ <= kBusyMaximumAge100ns;
    if (sampleBusy) {
        // The counter reports the mean since its previous read.
        const auto busy = adapterBusy_->busyPercent();
        lastBusy_ = recent ? busy : std::nullopt;
        busySampledAt100ns_ = now100ns;
    } else if (!recent) {
        lastBusy_.reset();
    }
    diagnostics.gpuBusyPercent = lastBusy_.value_or(-1.0);
}

}  // namespace clipture
