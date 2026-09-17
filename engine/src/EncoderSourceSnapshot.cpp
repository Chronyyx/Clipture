#include "EncoderSourceSnapshot.hpp"

namespace clipture {
Microsoft::WRL::ComPtr<ID3D11Texture2D> EncoderSourceSnapshot::copyAndRetire(
    ID3D11Device* device, ID3D11DeviceContext* context,
    SharedTextureReader& reader, ID3D11Texture2D* source,
    const std::shared_ptr<GpuTextureReadState>& lifetime, std::string& error) {
    if (!device || !context || !source || !lifetime || !reader.active()) {
        error = "Encoder source snapshot requires an active guarded reader.";
        return {};
    }
    Microsoft::WRL::ComPtr<ID3D11Device> owner, contextOwner;
    source->GetDevice(&owner);
    context->GetDevice(&contextOwner);
    D3D11_TEXTURE2D_DESC desc {};
    source->GetDesc(&desc);
    if (owner.Get() != device || contextOwner.Get() != device ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1 ||
        desc.MipLevels != 1 || desc.ArraySize != 1) {
        error = "Encoder source snapshot requires same-device single-sample BGRA.";
        return {};
    }
    D3D11_TEXTURE2D_DESC existing {};
    Microsoft::WRL::ComPtr<ID3D11Device> existingOwner;
    if (texture_) { texture_->GetDesc(&existing); texture_->GetDevice(&existingOwner); }
    if (!texture_ || existingOwner.Get() != device || existing.Width != desc.Width ||
        existing.Height != desc.Height) {
        // Allocate before arming the read guard. Failure cannot strand a slot.
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = desc.MiscFlags = 0;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> next;
        const auto hr = device->CreateTexture2D(&desc, nullptr, &next);
        if (FAILED(hr)) {
            error = "Encoder source snapshot allocation failed: " + std::to_string(hr);
            return {};
        }
        texture_ = next;
    }
    if (!reader.begin(lifetime, error)) return {};
    {
        auto sample = copyProbe_.scope(context);
        context->CopyResource(texture_.Get(), source);
    }
    // No later operation in this mode reads the shared capture texture.
    // end() signals AFTER the copy and flushes, never signals from the CPU.
    if (!reader.end(error)) return {};
    return texture_;
}
}
