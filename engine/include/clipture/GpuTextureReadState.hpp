#pragma once
#include <d3d11_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <mutex>
#include <vector>

namespace clipture {
// A capture slot is reusable only after both its CPU lease and all GPU readers
// finish. Completion survives encoder-session destruction; never waits on GPU.
class GpuTextureReadState {
    struct Read { Microsoft::WRL::ComPtr<ID3D11Fence> fence; UINT64 value; };
public:
    void retainUntil(ID3D11Fence* fence, UINT64 value) {
        std::lock_guard lock(mutex_);
        collect();
        for (auto& read : reads_) if (read.fence.Get() == fence) {
            read.value = std::max(read.value, value);
            return;
        }
        reads_.push_back({fence, value});
    }
    bool reusable() {
        std::lock_guard lock(mutex_);
        collect();
        return reads_.empty();
    }
private:
    void collect() {
        std::erase_if(reads_, [](const Read& read) {
            // UINT64_MAX means device removed: that device cannot keep reading.
            return read.fence->GetCompletedValue() >= read.value;
        });
    }
    std::mutex mutex_;
    std::vector<Read> reads_;
};
}
