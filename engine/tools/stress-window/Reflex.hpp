#pragma once

#include <d3d11.h>
#include <windows.h>

#include <cstdint>
#include <string>

namespace clipture::stress {

// NVIDIA Reflex low-latency mode, as CS2 uses it ("On + Boost"), through the
// driver's NVAPI entry points loaded at runtime. Each frame calls Sleep, which
// holds the CPU until the GPU is nearly idle so the render queue stays empty.
// No NVAPI SDK is needed: functions are fetched by their published interface
// IDs.
class Reflex {
public:
    bool enable(ID3D11Device* device, bool boost, std::string& error) {
        module_ = LoadLibraryW(L"nvapi64.dll");
        if (!module_) { error = "nvapi64.dll not found (NVIDIA driver required)"; return false; }
        const auto query = reinterpret_cast<QueryInterface>(GetProcAddress(module_, "nvapi_QueryInterface"));
        if (!query) { error = "nvapi_QueryInterface missing"; return false; }
        const auto initialize = reinterpret_cast<Initialize>(query(0x0150E828u));
        const auto setSleepMode = reinterpret_cast<SetSleepMode>(query(0xAC1CA9E0u));
        sleep_ = reinterpret_cast<SleepFunction>(query(0x852CD1D2u));
        if (!initialize || !setSleepMode || !sleep_) { error = "NVAPI Reflex entry points missing"; return false; }
        if (const int status = initialize(); status != 0) {
            error = "NvAPI_Initialize failed: " + std::to_string(status);
            return false;
        }
        SleepModeParams params {};
        params.version = sizeof(SleepModeParams) | (1u << 16);
        params.lowLatencyMode = 1;
        params.lowLatencyBoost = boost ? 1 : 0;
        if (const int status = setSleepMode(device, &params); status != 0) {
            error = "NvAPI_D3D_SetSleepMode failed: " + std::to_string(status);
            return false;
        }
        device_ = device;
        return true;
    }

    // Once per frame, before simulating and rendering it.
    void sleep() const {
        if (sleep_ && device_) sleep_(device_);
    }

    ~Reflex() {
        if (module_) FreeLibrary(module_);
    }

private:
    struct SleepModeParams {
        uint32_t version;
        uint8_t lowLatencyMode;
        uint8_t lowLatencyBoost;
        uint32_t minimumIntervalUs;
        uint8_t useMarkersToOptimize;
        uint8_t reserved[31];
    };
    static_assert(sizeof(SleepModeParams) == 44, "NV_SET_SLEEP_MODE_PARAMS_V1 layout");
    using QueryInterface = void*(__cdecl*)(uint32_t);
    using Initialize = int(__cdecl*)();
    using SetSleepMode = int(__cdecl*)(IUnknown*, SleepModeParams*);
    using SleepFunction = int(__cdecl*)(IUnknown*);

    HMODULE module_ = nullptr;
    SleepFunction sleep_ = nullptr;
    IUnknown* device_ = nullptr;
};

}  // namespace clipture::stress
