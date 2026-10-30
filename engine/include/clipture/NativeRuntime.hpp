#pragma once

#include <wrl/client.h>

#include <string>

struct IDXGIAdapter1;

namespace clipture {

struct NativeRuntimeState {
    bool d3d11Ready = false;
    bool captureReady = false;
    bool audioReady = false;
    bool muxReady = false;
    std::string adapterName = "Unknown";
    std::string resolution = "Native monitor";
    std::string status;
};

NativeRuntimeState initializeNativeRuntime(bool requireNvenc);

// The NVIDIA adapter if present, else the first hardware adapter.
Microsoft::WRL::ComPtr<IDXGIAdapter1> findPreferredAdapter(std::string& adapterName);

}  // namespace clipture
