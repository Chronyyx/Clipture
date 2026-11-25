#include "GameFrameProbe.hpp"

#include <dxgi.h>

#include <cstdint>

namespace clipture::capture {
namespace {
constexpr UINT kPatch = 32;
constexpr UINT kPatches = 5;

// Creates or reuses a staging texture for the five patches in `format`.
bool ensureStaging(ID3D11Device* device, DXGI_FORMAT format, Microsoft::WRL::ComPtr<ID3D11Texture2D>& staging) {
    D3D11_TEXTURE2D_DESC current {};
    if (staging) staging->GetDesc(&current);
    if (staging && current.Format == format) return true;
    D3D11_TEXTURE2D_DESC stagingDesc {};
    stagingDesc.Width = kPatch * kPatches;
    stagingDesc.Height = kPatch;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = format;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.Reset();
    return SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging));
}

// Queues the copy of the five patches; returns false if `frame` cannot be probed.
bool copyPatches(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame, UINT width, UINT height,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& staging, bool& half) {
    if (!frame || width < kPatch * 2 || height < kPatch * 2) return false;
    D3D11_TEXTURE2D_DESC desc {};
    frame->GetDesc(&desc);
    half = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!half && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return false;
    if (!ensureStaging(device, desc.Format, staging)) return false;

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
    return true;
}
}  // namespace

bool patchesHaveContent(const uint8_t* data, UINT rowPitch, UINT width, UINT height, bool half) {
    for (UINT y = 0; y < height; ++y) {
        const auto* row = data + size_t(y) * rowPitch;
        for (UINT x = 0; x < width; ++x) {
            if (half) {
                // Any colour channel above ~0.008 in scRGB.
                const auto* pixel = reinterpret_cast<const uint16_t*>(row) + size_t(x) * 4;
                if ((pixel[0] & 0x7FFF) > 0x2000 || (pixel[1] & 0x7FFF) > 0x2000 || (pixel[2] & 0x7FFF) > 0x2000) return true;
            } else {
                const auto* pixel = row + size_t(x) * 4;
                if (pixel[0] > 8 || pixel[1] > 8 || pixel[2] > 8) return true;
            }
        }
    }
    return false;
}

bool gameFrameHasContent(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame,
    UINT width, UINT height, Microsoft::WRL::ComPtr<ID3D11Texture2D>& staging) {
    bool half = false;
    if (!copyPatches(device, context, frame, width, height, staging, half)) return false;
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const bool content = patchesHaveContent(
        static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, kPatch * kPatches, kPatch, half);
    context->Unmap(staging.Get(), 0);
    return content;
}

bool GameFrameWatch::request(
    ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame, UINT width, UINT height) {
    if (pending_) return false;
    pending_ = copyPatches(device, context, frame, width, height, staging_, half_);
    return pending_;
}

std::optional<bool> GameFrameWatch::poll(ID3D11DeviceContext* context) {
    if (!pending_) return std::nullopt;
    D3D11_MAPPED_SUBRESOURCE mapped {};
    const HRESULT result = context->Map(staging_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) return std::nullopt;
    pending_ = false;
    // A failed map is no evidence either way: treat it as content, so only
    // frames actually read back as black can end game-window recording.
    if (FAILED(result)) return true;
    const bool content = patchesHaveContent(
        static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, kPatch * kPatches, kPatch, half_);
    context->Unmap(staging_.Get(), 0);
    return content;
}

}  // namespace clipture::capture
