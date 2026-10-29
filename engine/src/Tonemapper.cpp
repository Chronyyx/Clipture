#include "clipture/Tonemapper.hpp"
#include "clipture/MediaClock.hpp"
#include "GpuStageProbe.hpp"
#include <d3dcompiler.h>
#include <cstring>
#include <iostream>

namespace clipture {

// Windows composes SDR content into scRGB with the piecewise sRGB curve scaled
// to the SDR white level, so dividing by that level and re-applying the sRGB
// curve gives SDR pixels back exactly. Above the knee, luminance rolls off
// towards SDR white (ToneCurveControl); knee 1 leaves SDR content untouched.
const char* TonemapShaderCode = R"(
Texture2D<float4> InputTexture : register(t0);
RWTexture2D<unorm float4> OutputTexture : register(u0);
RWByteAddressBuffer Peak : register(u1);

cbuffer Curve : register(b0) {
    float InvWhite;
    float Knee;
    uint Width;
    uint Height;
};

groupshared uint GroupPeak;

float3 srgbEncode(float3 c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
}

[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    if (groupIndex == 0) GroupPeak = 0;
    GroupMemoryBarrierWithGroupSync();

    const bool inside = DTid.x < Width && DTid.y < Height;
    float4 hdrColor = InputTexture[DTid.xy];
    float3 scaledColor = max(hdrColor.rgb, 0.0f) * InvWhite;
    // Non-negative floats order like their bit patterns. Padding beyond the
    // content (WGC surfaces can be larger) is never measured.
    if (inside) InterlockedMax(GroupPeak, asuint(max(scaledColor.r, max(scaledColor.g, scaledColor.b))));

    float L = dot(scaledColor, float3(0.2126f, 0.7152f, 0.0722f));
    [branch]
    if (Knee < 1.0f && L > Knee) {
        float span = 1.0f - Knee;
        float L_new = Knee + span * (1.0f - exp(-(L - Knee) / span));
        scaledColor = scaledColor * (L_new / L);
    }
    if (inside) OutputTexture[DTid.xy] = float4(srgbEncode(saturate(scaledColor)), hdrColor.a);

    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0) Peak.InterlockedMax(0, GroupPeak);
}
)";

namespace {
struct CurveConstants {
    float invWhite;
    float knee;
    UINT width;
    UINT height;
};
static_assert(sizeof(CurveConstants) == 16);
}  // namespace

Tonemapper::Tonemapper(Microsoft::WRL::ComPtr<ID3D11Device> device)
    : device_(device) {
    device_->GetImmediateContext(&context_);
    if (pipelineTimingEnabled()) timingProbe_ = std::make_unique<GpuStageProbe>("tone-map");
}

Tonemapper::~Tonemapper() {}

bool Tonemapper::Initialize(std::string& errorMsg, float sdrWhiteLevel) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG;
#endif

    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(
        TonemapShaderCode,
        strlen(TonemapShaderCode),
        "TonemapCS",
        nullptr,
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

    invWhite_ = sdrWhiteLevel > 0.0f ? 1.0f / sdrWhiteLevel : 1.0f;
    D3D11_BUFFER_DESC constants { sizeof(CurveConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
        D3D11_CPU_ACCESS_WRITE, 0, 0 };
    D3D11_BUFFER_DESC peak { 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0 };
    D3D11_UNORDERED_ACCESS_VIEW_DESC peakView {};
    peakView.Format = DXGI_FORMAT_R32_TYPELESS;
    peakView.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    peakView.Buffer.NumElements = 1;
    peakView.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(device_->CreateBuffer(&constants, nullptr, &constants_)) ||
        FAILED(device_->CreateBuffer(&peak, nullptr, &peak_)) ||
        FAILED(device_->CreateUnorderedAccessView(peak_.Get(), &peakView, &peakView_))) {
        errorMsg = "Tone-map curve buffers could not be created.";
        return false;
    }
    D3D11_BUFFER_DESC staging { 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
    for (auto& readback : readbacks_) {
        if (FAILED(device_->CreateBuffer(&staging, nullptr, &readback.staging))) {
            errorMsg = "Tone-map peak readback could not be created.";
            return false;
        }
    }
    return true;
}

// Oldest first, without waiting: a peak not yet available stays queued.
void Tonemapper::readPeaks(int64_t now100ns) {
    while (true) {
        PeakReadback* oldest = nullptr;
        for (auto& readback : readbacks_) {
            if (readback.order && (!oldest || readback.order < oldest->order)) oldest = &readback;
        }
        if (!oldest) return;
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (context_->Map(oldest->staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK) return;
        float peak = 0.0f;
        std::memcpy(&peak, mapped.pData, sizeof(peak));
        context_->Unmap(oldest->staging.Get(), 0);
        oldest->order = 0;
        control_.observePeak(peak, now100ns);
    }
}

bool Tonemapper::Process(
    ID3D11ShaderResourceView* inputFloat16,
    ID3D11UnorderedAccessView* outputUnorm8,
    UINT width,
    UINT height,
    std::string& errorMsg
) {
    GpuStageProbe::Scope sample(timingProbe_.get(), context_.Get());
    if (!computeShader_) {
        errorMsg = "Tonemapper not initialized";
        return false;
    }
    if (!inputFloat16 || !outputUnorm8) {
        errorMsg = "Tonemapper input or output view is null";
        return false;
    }

    const int64_t now = monotonicNow100ns();
    readPeaks(now);
    const float knee = heldKnee_ > 0.0f ? heldKnee_ : control_.knee(now);
    const bool hdr = control_.hdrContent(now);
    if (hdr != hdrLogged_) {
        hdrLogged_ = hdr;
        std::cerr << (hdr ? "[capture] HDR content above SDR white: highlights roll off.\n"
                          : "[capture] SDR content only: exact SDR mapping.\n");
    }
    if (knee != appliedKnee_ || width != appliedWidth_ || height != appliedHeight_) {
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            errorMsg = "Tone-map curve update failed";
            return false;
        }
        const CurveConstants values { invWhite_, knee, width, height };
        std::memcpy(mapped.pData, &values, sizeof(values));
        context_->Unmap(constants_.Get(), 0);
        appliedKnee_ = knee;
        appliedWidth_ = width;
        appliedHeight_ = height;
    }

    const UINT zero[4] {};
    context_->ClearUnorderedAccessViewUint(peakView_.Get(), zero);
    context_->CSSetShader(computeShader_.Get(), nullptr, 0);
    ID3D11Buffer* constants[] = { constants_.Get() };
    context_->CSSetConstantBuffers(0, 1, constants);
    ID3D11ShaderResourceView* srvs[] = { inputFloat16 };
    context_->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView* uavs[] = { outputUnorm8, peakView_.Get() };
    context_->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

    // 16x16 threads per group for maximum GPU occupancy.
    context_->Dispatch((width + 15) / 16, (height + 15) / 16, 1);

    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    context_->CSSetShaderResources(0, 1, nullSRV);
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr, nullptr };
    context_->CSSetUnorderedAccessViews(0, 2, nullUAV, nullptr);
    ID3D11Buffer* nullConstants[] = { nullptr };
    context_->CSSetConstantBuffers(0, 1, nullConstants);

    // With every readback still in flight this frame goes unmeasured.
    for (auto& readback : readbacks_) {
        if (readback.order) continue;
        context_->CopyResource(readback.staging.Get(), peak_.Get());
        readback.order = nextOrder_++;
        break;
    }
    return true;
}

bool Tonemapper::Process(
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputFloat16,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputUnorm8,
    std::string& errorMsg
) {
    if (!inputFloat16 || !outputUnorm8) {
        errorMsg = "Tonemapper input or output texture is null";
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> inputSRV;
    HRESULT hr = device_->CreateShaderResourceView(inputFloat16.Get(), nullptr, &inputSRV);
    if (FAILED(hr)) {
        errorMsg = "CreateShaderResourceView failed: " + std::to_string(hr);
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputUAV;
    hr = device_->CreateUnorderedAccessView(outputUnorm8.Get(), nullptr, &outputUAV);
    if (FAILED(hr)) {
        errorMsg = "CreateUnorderedAccessView failed: " + std::to_string(hr);
        return false;
    }
    D3D11_TEXTURE2D_DESC outDesc;
    outputUnorm8->GetDesc(&outDesc);
    return Process(inputSRV.Get(), outputUAV.Get(), outDesc.Width, outDesc.Height, errorMsg);
}

} // namespace clipture
