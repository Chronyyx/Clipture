#include "clipture/platform/windows/GpuSchedulingPriority.hpp"

#include <algorithm>
#include <cwctype>

namespace clipture::platform {
namespace {

// gdi32 exports these; d3dkmthk.h declares them with the same signatures.
using SetClassFn = LONG(APIENTRY*)(HANDLE, int);
using GetClassFn = LONG(APIENTRY*)(HANDLE, int*);

struct Exports {
    SetClassFn set = nullptr;
    GetClassFn get = nullptr;
};

const Exports& exports() {
    static const Exports value = [] {
        Exports result;
        if (const HMODULE gdi = LoadLibraryW(L"gdi32.dll")) {
            result.set = reinterpret_cast<SetClassFn>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"));
            result.get = reinterpret_cast<GetClassFn>(GetProcAddress(gdi, "D3DKMTGetProcessSchedulingPriorityClass"));
        }
        return result;
    }();
    return value;
}

}  // namespace

bool parseGpuSchedulingClass(const std::wstring& name, GpuSchedulingClass& out) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (lower == L"idle") out = GpuSchedulingClass::Idle;
    else if (lower == L"belownormal") out = GpuSchedulingClass::BelowNormal;
    else if (lower == L"normal") out = GpuSchedulingClass::Normal;
    else if (lower == L"abovenormal") out = GpuSchedulingClass::AboveNormal;
    else if (lower == L"high") out = GpuSchedulingClass::High;
    else if (lower == L"realtime") out = GpuSchedulingClass::Realtime;
    else return false;
    return true;
}

GpuSchedulingResult setProcessGpuSchedulingClass(HANDLE process, GpuSchedulingClass schedulingClass) {
    GpuSchedulingResult result;
    const auto& calls = exports();
    if (!calls.set || !calls.get) return result;
    result.available = true;
    calls.get(process, &result.previous);
    result.setStatus = calls.set(process, static_cast<int>(schedulingClass));
    calls.get(process, &result.active);
    return result;
}

const char* gpuSchedulingClassName(int schedulingClass) {
    switch (schedulingClass) {
        case 0: return "idle";
        case 1: return "belownormal";
        case 2: return "normal";
        case 3: return "abovenormal";
        case 4: return "high";
        case 5: return "realtime";
        default: return "unknown";
    }
}

}  // namespace clipture::platform
