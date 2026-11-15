// Live check of the packets-in-flight sampler on this PC's GPU: it reads every
// engine node on the first sample of a pass and, from the second pass on,
// leaves the unused nodes out of the later samples.
#include "clipture/platform/windows/GpuAdapterFacts.hpp"

#include <Windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

ComPtr<IDXGIAdapter1> hardwareAdapter() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) return nullptr;
        DXGI_ADAPTER_DESC1 desc {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return adapter;
    }
}

}  // namespace

int main() {
    try {
        const auto adapter = hardwareAdapter();
        if (!adapter) {
            std::cout << "SKIP: no hardware GPU adapter" << std::endl;
            return 77;
        }
        constexpr int kSamples = 5;
        clipture::platform::PacketsInFlightSampler sampler(adapter.Get());
        const auto first = sampler.mean(kSamples, 1);
        if (!first) {
            std::cout << "SKIP: GPU statistics unavailable" << std::endl;
            return 77;
        }
        const int allNodes = sampler.lastNodeReads() / kSamples;
        require(allNodes > 0 && sampler.lastNodeReads() == allNodes * kSamples,
            "the first pass knows no history and reads every node on every sample");
        require(*first >= 0.0, "packets in flight cannot be negative");

        const auto second = sampler.mean(kSamples, 1);
        require(second.has_value() && *second >= 0.0, "a later pass still yields a mean");
        require(sampler.lastNodeReads() >= allNodes, "every node is read on a pass's first sample");
        require(sampler.lastNodeReads() <= allNodes * kSamples, "a later pass never reads more than every node");
        std::cout << "nodes=" << allNodes << " reads: first pass " << allNodes * kSamples
                  << ", later pass " << sampler.lastNodeReads() << "; in flight " << *first << " then " << *second
                  << std::endl;

        require(!clipture::platform::PacketsInFlightSampler(nullptr).mean(kSamples, 1).has_value(),
            "no adapter gives no reading");
        require(!sampler.mean(0, 1).has_value(), "no samples give no reading");
        std::cout << "PASS: packets in flight sampler" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
