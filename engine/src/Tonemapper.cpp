#include "clipture/Tonemapper.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <iterator>

namespace clipture {

constexpr std::size_t kMaximumCachedTonemapperViews = 16;

const char* TonemapShaderCode = R"(
Texture2D<float4> InputTexture : register(t0);
RWTexture2D<unorm float4> OutputTexture : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    float4 hdrColor = InputTexture[DTid.xy];
    float3 linearColor = max(hdrColor.rgb, 0.0f);
    
    // Normalize to SDR White Level (e.g., 400 nits -> 1.0)
    float3 scaledColor = linearColor * (1.0f / SDR_WHITE_LEVEL);
    
    // Calculate pure luminance (Rec.709)
    float L = dot(scaledColor, float3(0.2126f, 0.7152f, 0.0722f));
    
    [branch]
    if (L > 0.75f) {
        // Smooth exponential roll-off for HDR highlights (75% threshold)
        float L_new = 0.75f + 0.25f * (1.0f - exp(-(L - 0.75f) * 4.0f));
        scaledColor = scaledColor * (L_new / L);
    }
    
    // Fast standard gamma 2.2 transfer curve (OBS standard)
    float3 sdrColor = saturate(pow(max(scaledColor, 0.0f), 0.45454545f));
    
    OutputTexture[DTid.xy] = float4(sdrColor, hdrColor.a);
}
)";

bool sameTextureDesc(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) {
    return a.Width == b.Width &&
        a.Height == b.Height &&
        a.MipLevels == b.MipLevels &&
        a.ArraySize == b.ArraySize &&
        a.Format == b.Format &&
        a.SampleDesc.Count == b.SampleDesc.Count &&
        a.SampleDesc.Quality == b.SampleDesc.Quality &&
        a.Usage == b.Usage &&
        a.BindFlags == b.BindFlags &&
        a.CPUAccessFlags == b.CPUAccessFlags &&
        a.MiscFlags == b.MiscFlags;
}

Tonemapper::Tonemapper(Microsoft::WRL::ComPtr<ID3D11Device> device)
    : device_(device) {
    device_->GetImmediateContext(&context_);
}

Tonemapper::~Tonemapper() {}

bool Tonemapper::Initialize(std::string& errorMsg, float sdrWhiteLevel) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG;
#endif

    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    std::string sdrWhiteStr = std::to_string(sdrWhiteLevel) + "f";
    D3D_SHADER_MACRO macros[] = {
        { "SDR_WHITE_LEVEL", sdrWhiteStr.c_str() },
        { nullptr, nullptr }
    };

    HRESULT hr = D3DCompile(
        TonemapShaderCode,
        strlen(TonemapShaderCode),
        "TonemapCS",
        macros,
        nullptr,
        "main",
        "cs_5_0",
        flags,
        0,
        &shaderBlob,
        &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            errorMsg = std::string("D3DCompile failed: ") + static_cast<char*>(errorBlob->GetBufferPointer());
        } else {
            errorMsg = "D3DCompile failed with HRESULT: " + std::to_string(hr);
        }
        return false;
    }

    hr = device_->CreateComputeShader(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        nullptr,
        &computeShader_
    );

    if (FAILED(hr)) {
        errorMsg = "CreateComputeShader failed with HRESULT: " + std::to_string(hr);
        return false;
    }

    return true;
}

bool Tonemapper::Process(
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputFloat16,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputUnorm8,
    std::string& errorMsg
) {
    if (!computeShader_) {
        errorMsg = "Tonemapper not initialized";
        return false;
    }
    if (!inputFloat16 || !outputUnorm8) {
        errorMsg = "Tonemapper input or output texture is null";
        return false;
    }

    D3D11_TEXTURE2D_DESC inDesc;
    inputFloat16->GetDesc(&inDesc);
    auto inputViewIt = std::find_if(inputViews_.begin(), inputViews_.end(), [&](const InputViewCacheEntry& entry) {
        return entry.texture.Get() == inputFloat16.Get() && sameTextureDesc(entry.desc, inDesc);
    });
    if (inputViewIt == inputViews_.end()) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = inDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> inputSRV;
        HRESULT hr = device_->CreateShaderResourceView(inputFloat16.Get(), &srvDesc, &inputSRV);
        if (FAILED(hr)) {
            errorMsg = "CreateShaderResourceView failed: " + std::to_string(hr);
            return false;
        }
        if (inputViews_.size() >= kMaximumCachedTonemapperViews) inputViews_.erase(inputViews_.begin());
        inputViews_.push_back({ inputFloat16, inputSRV, inDesc });
        inputViewIt = std::prev(inputViews_.end());
    }

    D3D11_TEXTURE2D_DESC outDesc;
    outputUnorm8->GetDesc(&outDesc);
    auto outputViewIt = std::find_if(outputViews_.begin(), outputViews_.end(), [&](const OutputViewCacheEntry& entry) {
        return entry.texture.Get() == outputUnorm8.Get() && sameTextureDesc(entry.desc, outDesc);
    });
    if (outputViewIt == outputViews_.end()) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = outDesc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputUAV;
        HRESULT hr = device_->CreateUnorderedAccessView(outputUnorm8.Get(), &uavDesc, &outputUAV);
        if (FAILED(hr)) {
            errorMsg = "CreateUnorderedAccessView failed: " + std::to_string(hr);
            return false;
        }
        if (outputViews_.size() >= kMaximumCachedTonemapperViews) outputViews_.erase(outputViews_.begin());
        outputViews_.push_back({ outputUnorm8, outputUAV, outDesc });
        outputViewIt = std::prev(outputViews_.end());
    }

    // Dispatch compute shader
    context_->CSSetShader(computeShader_.Get(), nullptr, 0);
    
    ID3D11ShaderResourceView* srvs[] = { inputViewIt->view.Get() };
    context_->CSSetShaderResources(0, 1, srvs);
    
    ID3D11UnorderedAccessView* uavs[] = { outputViewIt->view.Get() };
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    // Calculate thread groups (16x16 threads per group for maximum GPU occupancy)
    UINT dispatchX = (outDesc.Width + 15) / 16;
    UINT dispatchY = (outDesc.Height + 15) / 16;
    context_->Dispatch(dispatchX, dispatchY, 1);

    // Unbind resources
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    context_->CSSetShaderResources(0, 1, nullSRV);
    
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    context_->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

    return true;
}

void Tonemapper::ResetViewCache() {
    inputViews_.clear();
    outputViews_.clear();
}

} // namespace clipture
