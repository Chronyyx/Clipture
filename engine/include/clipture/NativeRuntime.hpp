#pragma once

#include <string>

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

}  // namespace clipture
