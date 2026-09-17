#pragma once
#include <d3d11_4.h>
#include <wrl/client.h>
#include <string>
#include "GpuStageProbe.hpp"

namespace clipture {
// One producer/consumer texture, reused only after the consumer GPU has read
// it. All methods are called by the encoder submit worker. No CPU fence waits.
class GpuFrameHandoff {
public:
    bool initialize(ID3D11Device* producer, ID3D11Device* consumer);
    bool begin(ID3D11Texture2D* destination, ID3D11Texture2D* source, std::string& error);
    bool end(std::string& error);
    bool active() const { return producerContext_ && consumerContext_; }
private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> producerContext_, consumerContext_;
    Microsoft::WRL::ComPtr<ID3D11Fence> readyProducer_, readyConsumer_, doneProducer_, doneConsumer_;
    UINT64 sequence_ = 0;
    GpuStageProbe producerWaitProbe_ {"bridge-producer-wait"};
    GpuStageProbe copyProbe_ {"bridge-copy"};
    GpuStageProbe consumerWaitProbe_ {"bridge-consumer-wait"};
};
}
