#pragma once
#include "clipture/GpuTextureReadState.hpp"
#include "GpuStageProbe.hpp"
#include <memory>
#include <string>
#include <vector>

namespace clipture {
// Encoder-thread-owned direct reader. Producer only signals readiness: never
// queues a consumer-dependent Wait on the capture context and never copies to
// a reusable bridge. The source pool enforces GPU-safe reuse nonblockingly.
class SharedTextureReader {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Opened {
        Ptr<ID3D11Texture2D> source, texture;
        std::weak_ptr<GpuTextureReadState> lifetime;
    };
public:
    bool initialize(ID3D11Device* producer, ID3D11Device* consumer);
    Ptr<ID3D11Texture2D> open(ID3D11Texture2D* source,
        const std::shared_ptr<GpuTextureReadState>& lifetime, std::string& error);
    bool begin(const std::shared_ptr<GpuTextureReadState>& lifetime, std::string& error);
    bool end(std::string& error);
    bool active() const { return readyProducer_ && readyConsumer_ && consumed_; }
private:
    Ptr<ID3D11Device> producer_, consumer_;
    Ptr<ID3D11DeviceContext4> producerContext_, consumerContext_;
    Ptr<ID3D11Fence> readyProducer_, readyConsumer_, consumed_;
    UINT64 sequence_ = 0;
    std::vector<Opened> opened_;
    GpuStageProbe consumerWaitProbe_ {"source-consumer-wait"};
};
}
