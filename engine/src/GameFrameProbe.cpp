#include "GameFrameProbe.hpp"

#include <cstdint>

namespace clipture::capture {
namespace {
constexpr UINT kPatch = 32;
constexpr UINT kPatches = 5;
}  // namespace

bool gameFrameHasContent(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame,
    UINT width, UINT height, Microsoft::WRL::ComPtr<ID3D11Texture2D>& staging) {
    if (!frame || width < kPatch * 2 || height < kPatch * 2) return false;
    D3D11_TEXTURE2D_DESC desc {};
    frame->GetDesc(&desc);
    const bool half = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!half && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return false;

    D3D11_TEXTURE2D_DESC current {};
    if (staging) staging->GetDesc(&current);
    if (!staging || current.Format != desc.Format) {
        D3D11_TEXTURE2D_DESC stagingDesc {};
        stagingDesc.Width = kPatch * kPatches;
        stagingDesc.Height = kPatch;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = desc.Format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.Reset();
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) return false;
    }

    const UINT centres[kPatches][2] {
        {width / 2, height / 2}, {width / 4, height / 4}, {width * 3 / 4, height / 4},
        {width / 4, height * 3 / 4}, {width * 3 / 4, height * 3 / 4},
    };
    for (UINT i = 0; i < kPatches; ++i) {
        const UINT left = centres[i][0] - kPatch / 2;
        const UINT top = centres[i][1] - kPatch / 2;
        const D3D11_BOX box {left, top, 0, left + kPatch, top + kPatch, 1};
        context->CopySubresourceRegion(staging.Get(), 0, i * kPatch, 0, 0, frame, 0, &box);
    }

    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    bool content = false;
    for (UINT y = 0; y < kPatch && !content; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
        for (UINT x = 0; x < kPatch * kPatches && !content; ++x) {
            if (half) {
                // Any colour channel above ~0.008 in scRGB.
                const auto* pixel = reinterpret_cast<const uint16_t*>(row) + size_t(x) * 4;
                content = (pixel[0] & 0x7FFF) > 0x2000 || (pixel[1] & 0x7FFF) > 0x2000 || (pixel[2] & 0x7FFF) > 0x2000;
            } else {
                const auto* pixel = row + size_t(x) * 4;
                content = pixel[0] > 8 || pixel[1] > 8 || pixel[2] > 8;
            }
        }
    }
    context->Unmap(staging.Get(), 0);
    return content;
}

}  // namespace clipture::capture
