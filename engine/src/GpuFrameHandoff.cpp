#include "GpuFrameHandoff.hpp"
#include "GpuStageProbe.hpp"

namespace clipture {
using Microsoft::WRL::ComPtr;
namespace {
bool sharedFence(ID3D11Device5* owner, ID3D11Device5* peer,
                 ComPtr<ID3D11Fence>& local, ComPtr<ID3D11Fence>& remote) {
    if (FAILED(owner->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&local)))) return false;
    HANDLE handle = nullptr;
    if (FAILED(local->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle))) return false;
    const auto hr = peer->OpenSharedFence(handle, IID_PPV_ARGS(&remote));
    CloseHandle(handle); // NT fence handle, unlike a legacy texture share handle.
    return SUCCEEDED(hr);
}
}

bool GpuFrameHandoff::initialize(ID3D11Device* producer, ID3D11Device* consumer) {
    *this = {};
    if (!producer || !consumer) return false;
    ComPtr<ID3D11Device5> producer5, consumer5;
    ComPtr<ID3D11DeviceContext> producerContext, consumerContext;
    if (FAILED(producer->QueryInterface(IID_PPV_ARGS(&producer5))) ||
        FAILED(consumer->QueryInterface(IID_PPV_ARGS(&consumer5)))) return false;
    producer->GetImmediateContext(&producerContext);
    consumer->GetImmediateContext(&consumerContext);
    // Build locally: a failed capability check must never leave active() true.
    GpuFrameHandoff next;
    if (FAILED(producerContext.As(&next.producerContext_)) ||
        FAILED(consumerContext.As(&next.consumerContext_)) ||
        !sharedFence(producer5.Get(), consumer5.Get(), next.readyProducer_, next.readyConsumer_) ||
        !sharedFence(consumer5.Get(), producer5.Get(), next.doneConsumer_, next.doneProducer_)) return false;
    *this = std::move(next);
    return true;
}

bool GpuFrameHandoff::begin(ID3D11Texture2D* destination, ID3D11Texture2D* source, std::string& error) {
    if (!active() || !destination || !source) { error = "GPU handoff is not initialized."; return false; }
    // ID3D11DeviceContext4::Wait queues a GPU wait and returns immediately.
    // The consumer signals after conversion, NOT after NVENC bitstream output.
    HRESULT hr = S_OK;
    {
        auto sample = producerWaitProbe_.scope(producerContext_.Get());
        hr = sequence_ ? producerContext_->Wait(doneProducer_.Get(), sequence_) : S_OK;
    }
    if (SUCCEEDED(hr)) {
        {
            auto sample = copyProbe_.scope(producerContext_.Get());
            producerContext_->CopyResource(destination, source);
        }
        hr = producerContext_->Signal(readyProducer_.Get(), ++sequence_);
        producerContext_->Flush(); // Submit the signal before the peer queues its wait.
    }
    if (SUCCEEDED(hr)) {
        auto sample = consumerWaitProbe_.scope(consumerContext_.Get());
        hr = consumerContext_->Wait(readyConsumer_.Get(), sequence_);
    }
    if (FAILED(hr)) { error = "GPU producer handoff failed: " + std::to_string(hr); return false; }
    return true;
}

bool GpuFrameHandoff::end(std::string& error) {
    if (!active() || !sequence_) { error = "GPU handoff has no submitted image."; return false; }
    const auto hr = consumerContext_->Signal(doneConsumer_.Get(), sequence_);
    consumerContext_->Flush();
    if (FAILED(hr)) { error = "GPU consumer handoff failed: " + std::to_string(hr); return false; }
    return true;
}
}
