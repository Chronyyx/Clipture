#include "clipture/Tonemapper.hpp"
#include <d3dcompiler.h>
#include <iostream>

namespace clipture {

const char* TonemapShaderCode = R"(
Texture2D<float4> InputTexture : register(t0);
RWTexture2D<unorm float4> OutputTexture : register(u0);

// Exact sRGB piece-wise transfer function
float sRGBGamma(float v) {
    if (v <= 0.0031308f) {
        return 12.92f * v;
    }
    return 1.055f * pow(v, 1.0f / 2.4f) - 0.055f;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    float4 hdrColor = InputTexture[DTid.xy];
    float3 linearColor = max(hdrColor.rgb, 0.0f);
    
    // Normalize to SDR White Level (e.g., 400 nits -> 1.0)
    float3 scaledColor = linearColor / SDR_WHITE_LEVEL;
    
    // Calculate pure luminance
    float L = dot(scaledColor, float3(0.2126f, 0.7152f, 0.0722f));
    
    if (L > 0.0f) {
        float T = 0.75f; // Threshold: 75% of SDR brightness is perfectly preserved
        float L_new = L;
        
        if (L > T) {
            // Smooth exponential roll-off for HDR highlights
            L_new = T + (1.0f - T) * (1.0f - exp(-(L - T) / (1.0f - T)));
        }
        
        // Re-apply to the RGB ratios to preserve perfectly accurate hues
        scaledColor = scaledColor * (L_new / L);
    }
    
    // Apply exact sRGB transfer function
    float3 sdrColor;
    sdrColor.r = sRGBGamma(scaledColor.r);
    sdrColor.g = sRGBGamma(scaledColor.g);
    sdrColor.b = sRGBGamma(scaledColor.b);
    
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
    bool recreatedInputView = false;
    if (!cachedInputSRV_ ||
        cachedInputTexture_.Get() != inputFloat16.Get() ||
        !sameTextureDesc(cachedInputDesc_, inDesc)) {
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
        cachedInputTexture_ = inputFloat16;
        cachedInputSRV_ = inputSRV;
        cachedInputDesc_ = inDesc;
        recreatedInputView = true;
    }

    D3D11_TEXTURE2D_DESC outDesc;
    outputUnorm8->GetDesc(&outDesc);
    bool recreatedOutputView = false;
    if (!cachedOutputUAV_ ||
        cachedOutputTexture_.Get() != outputUnorm8.Get() ||
        !sameTextureDesc(cachedOutputDesc_, outDesc)) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = outDesc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputUAV;
        HRESULT hr = device_->CreateUnorderedAccessView(outputUnorm8.Get(), &uavDesc, &outputUAV);
        if (FAILED(hr)) {
            errorMsg = "CreateUnorderedAccessView failed: " + std::to_string(hr);
            return false;
        }
        cachedOutputTexture_ = outputUnorm8;
        cachedOutputUAV_ = outputUAV;
        cachedOutputDesc_ = outDesc;
        recreatedOutputView = true;
    }

    if (recreatedInputView || recreatedOutputView) {
        std::cerr << "[perf] tonemapper_views"
                  << " inputRecreated=" << (recreatedInputView ? "true" : "false")
                  << " outputRecreated=" << (recreatedOutputView ? "true" : "false")
                  << " size=" << outDesc.Width << "x" << outDesc.Height
                  << std::endl;
    }

    // Dispatch compute shader
    context_->CSSetShader(computeShader_.Get(), nullptr, 0);
    
    ID3D11ShaderResourceView* srvs[] = { cachedInputSRV_.Get() };
    context_->CSSetShaderResources(0, 1, srvs);
    
    ID3D11UnorderedAccessView* uavs[] = { cachedOutputUAV_.Get() };
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    // Calculate thread groups (8x8 threads per group)
    UINT dispatchX = (outDesc.Width + 7) / 8;
    UINT dispatchY = (outDesc.Height + 7) / 8;
    context_->Dispatch(dispatchX, dispatchY, 1);

    // Unbind resources
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    context_->CSSetShaderResources(0, 1, nullSRV);
    
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    context_->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

    return true;
}

} // namespace clipture
