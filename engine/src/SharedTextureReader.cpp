#include "SharedTextureReader.hpp"
#include <dxgi.h>

namespace clipture {
bool SharedTextureReader::initialize(ID3D11Device* producer, ID3D11Device* consumer) {
    *this = {};
    if (!producer || !consumer || producer == consumer) return false;
    SharedTextureReader next;
    Ptr<ID3D11Device5> p5, c5;
    Ptr<ID3D11DeviceContext> pc, cc;
    if (FAILED(producer->QueryInterface(IID_PPV_ARGS(&p5))) ||
        FAILED(consumer->QueryInterface(IID_PPV_ARGS(&c5)))) return false;
    producer->GetImmediateContext(&pc);
    consumer->GetImmediateContext(&cc);
    if (FAILED(pc.As(&next.producerContext_)) || FAILED(cc.As(&next.consumerContext_)) ||
        FAILED(p5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&next.readyProducer_))) ||
        FAILED(c5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&next.consumed_)))) return false;
    HANDLE handle = nullptr;
    if (FAILED(next.readyProducer_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle))) return false;
    const auto hr = c5->OpenSharedFence(handle, IID_PPV_ARGS(&next.readyConsumer_));
    CloseHandle(handle);
    if (FAILED(hr)) return false;
    next.producer_ = producer; next.consumer_ = consumer;
    *this = std::move(next);
    return true;
}

SharedTextureReader::Ptr<ID3D11Texture2D> SharedTextureReader::open(ID3D11Texture2D* source,
    const std::shared_ptr<GpuTextureReadState>& lifetime, std::string& error) {
    if (!active() || !source || !lifetime) { error = "Direct texture reader is not initialized."; return {}; }
    std::erase_if(opened_, [](const Opened& item) { return item.lifetime.expired(); });
    for (auto& item : opened_) if (item.source.Get() == source) return item.texture;
    Ptr<ID3D11Device> owner;
    source->GetDevice(&owner);
    if (owner.Get() != producer_.Get()) { error = "Direct source changed capture devices."; return {}; }
    Ptr<IDXGIResource> resource;
    HANDLE handle = nullptr;
    Ptr<ID3D11Texture2D> opened;
    if (FAILED(source->QueryInterface(IID_PPV_ARGS(&resource))) ||
        FAILED(resource->GetSharedHandle(&handle)) || !handle ||
        FAILED(consumer_->OpenSharedResource(handle, IID_PPV_ARGS(&opened)))) {
        error = "Direct capture texture sharing is unavailable.";
        return {};
    }
    // Legacy shared texture handles are not NT handles and must not be closed.
    opened_.push_back({source, opened, lifetime});
    return opened;
}

bool SharedTextureReader::begin(const std::shared_ptr<GpuTextureReadState>& lifetime, std::string& error) {
    if (!active() || !lifetime) { error = "Direct texture reader has no lifetime guard."; return false; }
    ++sequence_;
    // Arm before any consumer command. On errors the slot remains protected;
    // a later completion or device removal, not CPU lease release, frees it.
    lifetime->retainUntil(consumed_.Get(), sequence_);
    auto hr = producerContext_->Signal(readyProducer_.Get(), sequence_);
    producerContext_->Flush();
    if (SUCCEEDED(hr)) {
        auto sample = consumerWaitProbe_.scope(consumerContext_.Get());
        hr = consumerContext_->Wait(readyConsumer_.Get(), sequence_);
    }
    if (FAILED(hr)) { error = "Direct texture ready signal/wait failed: " + std::to_string(hr); return false; }
    return true;
}

bool SharedTextureReader::end(std::string& error) {
    if (!active() || !sequence_) { error = "Direct texture reader has no active read."; return false; }
    const auto hr = consumerContext_->Signal(consumed_.Get(), sequence_);
    consumerContext_->Flush();
    if (FAILED(hr)) { error = "Direct texture completion signal failed: " + std::to_string(hr); return false; }
    return true;
}
}
