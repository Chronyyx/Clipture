#include "clipture/GpuPriorityControl.hpp"

#include "clipture/NativeRuntime.hpp"
#include "clipture/platform/windows/GpuAdapterFacts.hpp"
#include "clipture/platform/windows/GpuSchedulingPriority.hpp"

#include <Windows.h>
#include <dxgi1_6.h>

#include <iostream>

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

}  // namespace

GpuPriorityControl::GpuPriorityControl() {
    std::string ignoredName;
    adapter_ = findPreferredAdapter(ignoredName);
    adapterMemory_ = std::make_unique<platform::AdapterMemoryCounter>(adapter_.Get());
}

GpuPriorityControl::~GpuPriorityControl() = default;

void GpuPriorityControl::apply(bool boost, Diagnostics& diagnostics) {
    if (!factsRecorded_) {
        const auto facts = platform::queryGpuAdapterFacts(adapter_.Get());
        diagnostics.gpuDriverVersion = facts.driverVersion;
        diagnostics.hardwareGpuScheduling = facts.hardwareScheduling;
        diagnostics.vramDedicatedMb = facts.dedicatedVideoMemoryBytes / kMegabyte;
        factsRecorded_ = true;
    }
    if (appliedBoost_ == boost) return;
    appliedBoost_ = boost;

    const auto override = environmentOverride();
    const auto requested = override.value_or(
        boost ? platform::GpuSchedulingClass::Realtime : platform::GpuSchedulingClass::Normal);
    const auto result = platform::setProcessGpuSchedulingClass(GetCurrentProcess(), requested);
    diagnostics.gpuPriorityBoost = boost;
    diagnostics.gpuSchedulingRequested = platform::gpuSchedulingClassName(static_cast<int>(requested));
    diagnostics.gpuSchedulingOverride = override.has_value();
    diagnostics.gpuSchedulingClass = result.available
        ? platform::gpuSchedulingClassName(result.active)
        : "unavailable";
    diagnostics.gpuSchedulingStatus = static_cast<int64_t>(result.setStatus);
    std::cerr << "[engine] GPU scheduling class requested=" << diagnostics.gpuSchedulingRequested
              << (override ? " (override)" : "")
              << " active=" << diagnostics.gpuSchedulingClass
              << " status=0x" << std::hex << static_cast<unsigned long>(result.setStatus) << std::dec
              << " hags=" << diagnostics.hardwareGpuScheduling
              << " driver=" << diagnostics.gpuDriverVersion << ".\n";
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
