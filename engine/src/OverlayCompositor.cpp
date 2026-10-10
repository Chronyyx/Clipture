#include "OverlayCompositor.hpp"

#include "CaptureBackend.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace clipture::capture {
namespace {

constexpr char kOverlayShader[] = R"(
cbuffer OverlayConstants : register(b0) {
    int2 LayerOrigin;
    int2 Padding;
};
Texture2D<float4> Layer : register(t0);

float4 VSMain(uint vertexId : SV_VertexID) : SV_POSITION {
    const float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    return float4(positions[vertexId], 0.0, 1.0);
}

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    return Layer.Load(int3(int2(position.xy) - LayerOrigin, 0));
}
)";

struct Constants {
    int32_t origin[2] {};
    int32_t padding[2] {};
};

bool compileShader(const char* entryPoint, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& blob, std::string& error) {
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kOverlayShader, sizeof(kOverlayShader) - 1, "CliptureOverlay",
        nullptr, nullptr, entryPoint, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (SUCCEEDED(hr)) return true;
    error = errors && errors->GetBufferPointer()
        ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
        : "D3DCompile failed: " + hresultHex(hr);
    return false;
}

}  // namespace

OverlayCompositor::OverlayCompositor(
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context)
    : device_(std::move(device)), context_(std::move(context)) {}

bool OverlayCompositor::initialize(std::string& error) {
    Microsoft::WRL::ComPtr<ID3DBlob> vertexBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelBlob;
    if (!compileShader("VSMain", "vs_5_0", vertexBlob, error) ||
        !compileShader("PSMain", "ps_5_0", pixelBlob, error)) {
        return false;
    }
    HRESULT hr = device_->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &vertexShader_);
    if (SUCCEEDED(hr)) hr = device_->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &pixelShader_);

    D3D11_BUFFER_DESC constantsDesc {};
    constantsDesc.ByteWidth = sizeof(Constants);
    constantsDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) hr = device_->CreateBuffer(&constantsDesc, nullptr, &constantBuffer_);

    D3D11_RASTERIZER_DESC rasterizerDesc {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = device_->CreateRasterizerState(&rasterizerDesc, &rasterizerState_);

    // WGC delivers premultiplied alpha; keep the frame's own alpha.
    D3D11_BLEND_DESC blendDesc {};
    auto& target = blendDesc.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_ONE;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ZERO;
    target.DestBlendAlpha = D3D11_BLEND_ONE;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = device_->CreateBlendState(&blendDesc, &blendState_);

    D3D11_DEPTH_STENCIL_DESC depthDesc {};
    if (SUCCEEDED(hr)) hr = device_->CreateDepthStencilState(&depthDesc, &depthStencilState_);
    if (FAILED(hr)) {
        error = "Overlay compositor setup failed: " + hresultHex(hr);
        return false;
    }
    return true;
}

bool OverlayCompositor::composite(const std::vector<OverlayLayer>& layers, ID3D11RenderTargetView* output,
    UINT outputWidth, UINT outputHeight, std::string& error) {
    if (layers.empty() || !output) return true;
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->RSSetState(rasterizerState_.Get());
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    constexpr float blendFactor[4] {};
    context_->OMSetBlendState(blendState_.Get(), blendFactor, 0xFFFFFFFFu);
    context_->OMSetDepthStencilState(depthStencilState_.Get(), 0);
    context_->OMSetRenderTargets(1, &output, nullptr);
    ID3D11Buffer* constants = constantBuffer_.Get();
    context_->PSSetConstantBuffers(0, 1, &constants);

    for (const auto& layer : layers) {
        const int left = std::max(layer.x, 0);
        const int top = std::max(layer.y, 0);
        const int right = std::min<int>(layer.x + static_cast<int>(layer.width), static_cast<int>(outputWidth));
        const int bottom = std::min<int>(layer.y + static_cast<int>(layer.height), static_cast<int>(outputHeight));
        if (!layer.view || right <= left || bottom <= top) continue;

        D3D11_MAPPED_SUBRESOURCE mapped {};
        const HRESULT hr = context_->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            error = "Map for overlay constants failed: " + hresultHex(hr);
            return false;
        }
        Constants values;
        values.origin[0] = layer.x;
        values.origin[1] = layer.y;
        std::memcpy(mapped.pData, &values, sizeof(values));
        context_->Unmap(constantBuffer_.Get(), 0);

        D3D11_VIEWPORT viewport {};
        viewport.TopLeftX = static_cast<float>(left);
        viewport.TopLeftY = static_cast<float>(top);
        viewport.Width = static_cast<float>(right - left);
        viewport.Height = static_cast<float>(bottom - top);
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        ID3D11ShaderResourceView* view = layer.view.Get();
        context_->PSSetShaderResources(0, 1, &view);
        context_->Draw(3, 0);
    }

    ID3D11ShaderResourceView* nullView = nullptr;
    ID3D11RenderTargetView* nullTarget = nullptr;
    context_->PSSetShaderResources(0, 1, &nullView);
    context_->OMSetRenderTargets(1, &nullTarget, nullptr);
    context_->VSSetShader(nullptr, nullptr, 0);
    context_->PSSetShader(nullptr, nullptr, 0);
    return true;
}

}  // namespace clipture::capture
