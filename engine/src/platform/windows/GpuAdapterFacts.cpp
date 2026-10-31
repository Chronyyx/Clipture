#include "clipture/platform/windows/GpuAdapterFacts.hpp"

#include <Windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <vector>

namespace clipture::platform {
namespace {

// gdi32 exports these; loaded at run time like the scheduling-class calls.
using OpenFromLuidFn = NTSTATUS(APIENTRY*)(D3DKMT_OPENADAPTERFROMLUID*);
using QueryInfoFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYADAPTERINFO*);
using CloseFn = NTSTATUS(APIENTRY*)(const D3DKMT_CLOSEADAPTER*);

std::string hardwareSchedulingState(const LUID& luid) {
    const HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) return "unknown";
    const auto open = reinterpret_cast<OpenFromLuidFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
    const auto query = reinterpret_cast<QueryInfoFn>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    const auto close = reinterpret_cast<CloseFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    if (!open || !query || !close) return "unknown";

    D3DKMT_OPENADAPTERFROMLUID adapter {};
    adapter.AdapterLuid = luid;
    if (open(&adapter) != 0) return "unknown";
    D3DKMT_WDDM_2_7_CAPS caps {};
    D3DKMT_QUERYADAPTERINFO info {};
    info.hAdapter = adapter.hAdapter;
    info.Type = KMTQAITYPE_WDDM_2_7_CAPS;
    info.pPrivateDriverData = &caps;
    info.PrivateDriverDataSize = sizeof(caps);
    const bool known = query(&info) == 0;
    D3DKMT_CLOSEADAPTER closing { adapter.hAdapter };
    close(&closing);
    if (!known) return "unknown";
    if (!caps.HwSchSupported) return "unsupported";
    return caps.HwSchEnabled ? "on" : "off";
}

std::string userModeDriverVersion(IDXGIAdapter1* adapter) {
    LARGE_INTEGER version {};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) return "unknown";
    std::ostringstream text;
    text << HIWORD(version.HighPart) << '.' << LOWORD(version.HighPart) << '.'
         << HIWORD(version.LowPart) << '.' << LOWORD(version.LowPart);
    return text.str();
}

}  // namespace

GpuAdapterFacts queryGpuAdapterFacts(IDXGIAdapter1* adapter) {
    GpuAdapterFacts facts;
    if (!adapter) return facts;
    DXGI_ADAPTER_DESC1 desc {};
    if (FAILED(adapter->GetDesc1(&desc))) return facts;
    facts.driverVersion = userModeDriverVersion(adapter);
    facts.hardwareScheduling = hardwareSchedulingState(desc.AdapterLuid);
    facts.dedicatedVideoMemoryBytes = desc.DedicatedVideoMemory;
    return facts;
}

VideoMemoryUsage queryVideoMemoryUsage(IDXGIAdapter1* adapter) {
    VideoMemoryUsage usage;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    if (!adapter || FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) return usage;
    DXGI_QUERY_VIDEO_MEMORY_INFO info {};
    if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return usage;
    usage.available = true;
    usage.budgetBytes = info.Budget;
    usage.processUsageBytes = info.CurrentUsage;
    return usage;
}

struct AdapterMemoryCounter::State {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    wchar_t luidTag[40] {};
};

AdapterMemoryCounter::AdapterMemoryCounter(IDXGIAdapter1* adapter) : state_(std::make_unique<State>()) {
    DXGI_ADAPTER_DESC1 desc {};
    if (!adapter || FAILED(adapter->GetDesc1(&desc))) return;
    // Counter instances are named "luid_0x<high>_0x<low>_phys_<n>".
    swprintf_s(state_->luidTag, L"luid_0x%08X_0x%08X",
        static_cast<unsigned>(desc.AdapterLuid.HighPart), static_cast<unsigned>(desc.AdapterLuid.LowPart));
    if (PdhOpenQueryW(nullptr, 0, &state_->query) != ERROR_SUCCESS) {
        state_->query = nullptr;
        return;
    }
    if (PdhAddEnglishCounterW(state_->query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &state_->counter) != ERROR_SUCCESS) {
        PdhCloseQuery(state_->query);
        state_->query = nullptr;
    }
}

AdapterMemoryCounter::~AdapterMemoryCounter() {
    if (state_ && state_->query) PdhCloseQuery(state_->query);
}

std::optional<uint64_t> AdapterMemoryCounter::dedicatedUsageBytes() {
    if (!state_->query || PdhCollectQueryData(state_->query) != ERROR_SUCCESS) return std::nullopt;
    DWORD bytes = 0;
    DWORD count = 0;
    if (PdhGetFormattedCounterArrayW(state_->counter, PDH_FMT_LARGE, &bytes, &count, nullptr) != PDH_MORE_DATA) {
        return std::nullopt;
    }
    std::vector<unsigned char> buffer(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(state_->counter, PDH_FMT_LARGE, &bytes, &count, items) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    uint64_t total = 0;
    bool matched = false;
    for (DWORD index = 0; index < count; ++index) {
        if (_wcsnicmp(items[index].szName, state_->luidTag, wcslen(state_->luidTag)) != 0) continue;
        if (items[index].FmtValue.CStatus != ERROR_SUCCESS) continue;
        total += static_cast<uint64_t>(items[index].FmtValue.largeValue);
        matched = true;
    }
    if (!matched) return std::nullopt;
    return total;
}

std::optional<double> meanPacketsInFlight(IDXGIAdapter1* adapter, int samples, int intervalMs) {
    using QueryStatisticsFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);
    static const auto queryStatistics = [] {
        const HMODULE gdi = LoadLibraryW(L"gdi32.dll");
        return gdi ? reinterpret_cast<QueryStatisticsFn>(GetProcAddress(gdi, "D3DKMTQueryStatistics")) : nullptr;
    }();
    DXGI_ADAPTER_DESC1 desc {};
    if (!queryStatistics || !adapter || FAILED(adapter->GetDesc1(&desc)) || samples <= 0) return std::nullopt;
    D3DKMT_QUERYSTATISTICS adapterQuery {};
    adapterQuery.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
    adapterQuery.AdapterLuid = desc.AdapterLuid;
    if (queryStatistics(&adapterQuery) != 0) return std::nullopt;
    const ULONG nodes = adapterQuery.QueryResult.AdapterInformation.NodeCount;

    double total = 0.0;
    int read = 0;
    for (int sample = 0; sample < samples; ++sample) {
        if (sample > 0) Sleep(static_cast<DWORD>(intervalMs));
        int64_t inFlight = 0;
        bool ok = true;
        for (ULONG node = 0; node < nodes && ok; ++node) {
            D3DKMT_QUERYSTATISTICS query {};
            query.Type = D3DKMT_QUERYSTATISTICS_NODE;
            query.AdapterLuid = desc.AdapterLuid;
            query.QueryNode.NodeId = node;
            ok = queryStatistics(&query) == 0;
            if (!ok) break;
            const auto& packets = query.QueryResult.NodeInformation.GlobalInformation.PacketStatistics;
            for (const auto& type : packets.QueuePacket) {
                inFlight += static_cast<int64_t>(type.PacketSubmited) - static_cast<int64_t>(type.PacketCompleted);
            }
        }
        if (!ok) continue;
        total += static_cast<double>(std::max<int64_t>(0, inFlight));
        ++read;
    }
    if (read == 0) return std::nullopt;
    return total / read;
}

struct AdapterBusyCounter::State {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    wchar_t luidTag[40] {};
    bool primed = false;
};

AdapterBusyCounter::AdapterBusyCounter(IDXGIAdapter1* adapter) : state_(std::make_unique<State>()) {
    DXGI_ADAPTER_DESC1 desc {};
    if (!adapter || FAILED(adapter->GetDesc1(&desc))) return;
    // Instances are named "pid_<n>_luid_0x<high>_0x<low>_phys_<n>_eng_<n>_engtype_<type>".
    swprintf_s(state_->luidTag, L"luid_0x%08X_0x%08X",
        static_cast<unsigned>(desc.AdapterLuid.HighPart), static_cast<unsigned>(desc.AdapterLuid.LowPart));
    if (PdhOpenQueryW(nullptr, 0, &state_->query) != ERROR_SUCCESS) {
        state_->query = nullptr;
        return;
    }
    if (PdhAddEnglishCounterW(state_->query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &state_->counter) != ERROR_SUCCESS) {
        PdhCloseQuery(state_->query);
        state_->query = nullptr;
    }
}

AdapterBusyCounter::~AdapterBusyCounter() {
    if (state_ && state_->query) PdhCloseQuery(state_->query);
}

std::optional<double> AdapterBusyCounter::busyPercent() {
    if (!state_->query || PdhCollectQueryData(state_->query) != ERROR_SUCCESS) return std::nullopt;
    if (!state_->primed) {
        state_->primed = true;
        return std::nullopt;
    }
    DWORD bytes = 0;
    DWORD count = 0;
    if (PdhGetFormattedCounterArrayW(state_->counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA) {
        return std::nullopt;
    }
    std::vector<unsigned char> buffer(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(state_->counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    double total = 0.0;
    bool matched = false;
    for (DWORD index = 0; index < count; ++index) {
        if (!wcsstr(items[index].szName, state_->luidTag)) continue;
        if (items[index].FmtValue.CStatus != ERROR_SUCCESS && items[index].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        total += items[index].FmtValue.doubleValue;
        matched = true;
    }
    if (!matched) return std::nullopt;
    return std::min(total, 100.0);
}

}  // namespace clipture::platform
