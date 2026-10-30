#pragma once
#include <Windows.h>
#include <string>

namespace clipture::platform {

// The process's GPU scheduling class (D3DKMT), which ranks every context this
// process creates against other processes' contexts, the foreground game's
// included. Per-device thread priority (IDXGIDevice::SetGPUThreadPriority)
// only ranks contexts within a priority band.
enum class GpuSchedulingClass : int {
    Idle = 0,
    BelowNormal = 1,
    Normal = 2,
    AboveNormal = 3,
    High = 4,
    Realtime = 5,
};

struct GpuSchedulingResult {
    bool available = false;  // gdi32 exports the D3DKMT calls
    long setStatus = 0;      // NTSTATUS of the set call; 0 is success
    int previous = -1;
    int active = -1;
};

// Parses idle, belownormal, normal, abovenormal, high or realtime.
bool parseGpuSchedulingClass(const std::wstring& name, GpuSchedulingClass& out);
GpuSchedulingResult setProcessGpuSchedulingClass(HANDLE process, GpuSchedulingClass schedulingClass);
const char* gpuSchedulingClassName(int schedulingClass);

}  // namespace clipture::platform
