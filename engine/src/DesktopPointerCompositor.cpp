#include "DesktopPointerCompositor.hpp"

#include "CaptureBackend.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace clipture::capture {
namespace {

constexpr char kPointerShader[] = R"(
cbuffer CursorConstants : register(b0) {
    float2 ViewportOrigin;
    uint2 CursorSourceOffset;
};

struct VertexOutput {
    float4 position : SV_POSITION;
};

VertexOutput VSMain(uint vertexId : SV_VertexID) {
    static const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    return output;
}

Texture2D<float4> BackgroundTexture : register(t0);
Texture2D<uint4> PointerTexture : register(t1);

float4 PSMain(VertexOutput input) : SV_TARGET {
    uint2 localPixel = uint2(input.position.xy - ViewportOrigin);
    float4 background = BackgroundTexture.Load(int3(localPixel, 0));
    uint4 operation = PointerTexture.Load(int3(localPixel + CursorSourceOffset, 0));
    uint mode = operation.a >> 8;
    uint alphaByte = operation.a & 255;
    uint3 sourceBytes = operation.rgb;
    uint3 backgroundBytes = uint3(round(saturate(background.rgb) * 255.0));
    uint3 resultBytes = backgroundBytes;

    if (mode == 1) {
        float alpha = alphaByte / 255.0;
        return float4(lerp(background.rgb, sourceBytes / 255.0, alpha), background.a);
    }
    if (mode == 2) {
        resultBytes = sourceBytes;
    } else if (mode == 3) {
        resultBytes = backgroundBytes ^ sourceBytes;
    } else if (mode == 4) {
        uint andMask = operation.r != 0 ? 255 : 0;
        uint xorMask = operation.g != 0 ? 255 : 0;
        resultBytes = (backgroundBytes & andMask) ^ xorMask;
    }
    return float4(resultBytes / 255.0, background.a);
}
)";

bool compileShader(const char* entryPoint, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& blob, std::string& error) {
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(
        kPointerShader,
        sizeof(kPointerShader) - 1,
        "CliptureDesktopPointer",
        nullptr,
        nullptr,
        entryPoint,
        target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &blob,
        &errors);
    if (SUCCEEDED(hr)) return true;
    if (errors && errors->GetBufferPointer()) {
        error.assign(
            static_cast<const char*>(errors->GetBufferPointer()),
            errors->GetBufferSize());
    } else {
        error = "D3DCompile failed: " + hresultHex(hr);
    }
    return false;
}

}  // namespace

struct DesktopPointerCompositor::Constants {
    float viewportOrigin[2] {};
    uint32_t cursorSourceOffset[2] {};
};

DesktopPointerCompositor::DesktopPointerCompositor(
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context)
    : device_(std::move(device)), context_(std::move(context)) {}

bool DesktopPointerCompositor::initialize(std::string& error) {
    Microsoft::WRL::ComPtr<ID3DBlob> vertexBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelBlob;
    if (!compileShader("VSMain", "vs_5_0", vertexBlob, error) ||
        !compileShader("PSMain", "ps_5_0", pixelBlob, error)) {
        return false;
    }

    HRESULT hr = device_->CreateVertexShader(
        vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &vertexShader_);
    if (FAILED(hr)) {
        error = "CreateVertexShader for desktop pointer failed: " + hresultHex(hr);
        return false;
    }
    hr = device_->CreatePixelShader(
        pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &pixelShader_);
    if (FAILED(hr)) {
        error = "CreatePixelShader for desktop pointer failed: " + hresultHex(hr);
        return false;
    }

    D3D11_BUFFER_DESC constantsDesc {};
    constantsDesc.ByteWidth = sizeof(Constants);
    constantsDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&constantsDesc, nullptr, &constantBuffer_);
    if (FAILED(hr)) {
        error = "CreateBuffer for desktop pointer failed: " + hresultHex(hr);
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDesc {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    hr = device_->CreateRasterizerState(&rasterizerDesc, &rasterizerState_);
    if (FAILED(hr)) {
        error = "CreateRasterizerState for desktop pointer failed: " + hresultHex(hr);
        return false;
    }

    D3D11_BLEND_DESC blendDesc {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device_->CreateBlendState(&blendDesc, &blendState_);
    if (FAILED(hr)) {
        error = "CreateBlendState for desktop pointer failed: " + hresultHex(hr);
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depthDesc {};
    depthDesc.DepthEnable = FALSE;
    depthDesc.StencilEnable = FALSE;
    hr = device_->CreateDepthStencilState(&depthDesc, &depthStencilState_);
    if (FAILED(hr)) {
        error = "CreateDepthStencilState for desktop pointer failed: " + hresultHex(hr);
        return false;
    }
    return true;
}

bool DesktopPointerCompositor::uploadShape(
    const DecodedDesktopPointerShape& decoded,
    std::string& error) {
    if (decoded.width == 0 || decoded.height == 0 || decoded.rgbaOperationPixels.empty()) {
        error = "Decoded desktop pointer shape is empty.";
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDesc {};
    textureDesc.Width = decoded.width;
    textureDesc.Height = decoded.height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R16G16B16A16_UINT;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initialData {};
    initialData.pSysMem = decoded.rgbaOperationPixels.data();
    initialData.SysMemPitch = decoded.width * 4 * sizeof(uint16_t);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> pointerTexture;
    HRESULT hr = device_->CreateTexture2D(&textureDesc, &initialData, &pointerTexture);
    if (FAILED(hr)) {
        error = "CreateTexture2D for desktop pointer failed: " + hresultHex(hr);
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pointerView;
    hr = device_->CreateShaderResourceView(pointerTexture.Get(), nullptr, &pointerView);
    if (FAILED(hr)) {
        error = "CreateShaderResourceView for desktop pointer failed: " + hresultHex(hr);
        return false;
    }

    pointerTexture_ = std::move(pointerTexture);
    pointerView_ = std::move(pointerView);
    pointerWidth_ = decoded.width;
    pointerHeight_ = decoded.height;
    return true;
}

bool DesktopPointerCompositor::update(
    IDXGIOutputDuplication* duplication,
    const DXGI_OUTDUPL_FRAME_INFO& frameInfo,
    std::string& error) {
    if (!duplication) return false;
    if (frameInfo.LastMouseUpdateTime.QuadPart != 0) {
        pointerPosition_ = frameInfo.PointerPosition.Position;
        pointerVisible_ = frameInfo.PointerPosition.Visible != FALSE;
    }
    if (frameInfo.PointerShapeBufferSize == 0) return true;

    shapeBytes_.resize(frameInfo.PointerShapeBufferSize);
    UINT requiredSize = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo {};
    HRESULT hr = duplication->GetFramePointerShape(
        static_cast<UINT>(shapeBytes_.size()),
        shapeBytes_.data(),
        &requiredSize,
        &shapeInfo);
    if (hr == DXGI_ERROR_MORE_DATA) {
        shapeBytes_.resize(requiredSize);
        hr = duplication->GetFramePointerShape(
            static_cast<UINT>(shapeBytes_.size()),
            shapeBytes_.data(),
            &requiredSize,
            &shapeInfo);
    }
    if (FAILED(hr)) {
        error = "GetFramePointerShape failed: " + hresultHex(hr);
        return false;
    }
    shapeBytes_.resize(requiredSize);

    DecodedDesktopPointerShape decoded;
    if (!decodeDesktopPointerShape(shapeInfo, shapeBytes_, decoded, error)) return false;
    return uploadShape(decoded, error);
}

bool DesktopPointerCompositor::ensureBackgroundTexture(UINT width, UINT height, std::string& error) {
    if (backgroundTexture_ && backgroundWidth_ >= width && backgroundHeight_ >= height) return true;

    const UINT allocationWidth = std::max(width, pointerWidth_);
    const UINT allocationHeight = std::max(height, pointerHeight_);

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = allocationWidth;
    desc.Height = allocationHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        error = "CreateTexture2D for pointer background failed: " + hresultHex(hr);
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    hr = device_->CreateShaderResourceView(texture.Get(), nullptr, &view);
    if (FAILED(hr)) {
        error = "CreateShaderResourceView for pointer background failed: " + hresultHex(hr);
        return false;
    }
    backgroundTexture_ = std::move(texture);
    backgroundView_ = std::move(view);
    backgroundWidth_ = allocationWidth;
    backgroundHeight_ = allocationHeight;
    return true;
}

bool DesktopPointerCompositor::composite(
    ID3D11Texture2D* desktopTexture,
    ID3D11RenderTargetView* outputView,
    UINT outputWidth,
    UINT outputHeight,
    std::string& error) {
    if (!pointerVisible_ || !pointerView_ || !desktopTexture || !outputView) return true;

    const auto clip = clipDesktopPointer(
        pointerPosition_, pointerWidth_, pointerHeight_, outputWidth, outputHeight);
    if (!clip) return true;
    const UINT clippedWidth = clip.width;
    const UINT clippedHeight = clip.height;
    const UINT sourceOffsetX = clip.sourceX;
    const UINT sourceOffsetY = clip.sourceY;
    if (!ensureBackgroundTexture(clippedWidth, clippedHeight, error)) return false;

    D3D11_BOX sourceBox {
        clip.destinationX,
        clip.destinationY,
        0,
        clip.destinationX + clip.width,
        clip.destinationY + clip.height,
        1,
    };
    context_->CopySubresourceRegion(backgroundTexture_.Get(), 0, 0, 0, 0, desktopTexture, 0, &sourceBox);

    D3D11_MAPPED_SUBRESOURCE mapped {};
    HRESULT hr = context_->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        error = "Map for pointer constants failed: " + hresultHex(hr);
        return false;
    }
    Constants constants;
    constants.viewportOrigin[0] = static_cast<float>(clip.destinationX);
    constants.viewportOrigin[1] = static_cast<float>(clip.destinationY);
    constants.cursorSourceOffset[0] = sourceOffsetX;
    constants.cursorSourceOffset[1] = sourceOffsetY;
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(constantBuffer_.Get(), 0);

    D3D11_VIEWPORT viewport {};
    viewport.TopLeftX = static_cast<float>(clip.destinationX);
    viewport.TopLeftY = static_cast<float>(clip.destinationY);
    viewport.Width = static_cast<float>(clippedWidth);
    viewport.Height = static_cast<float>(clippedHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizerState_.Get());
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    ID3D11Buffer* constantsBuffer = constantBuffer_.Get();
    context_->PSSetConstantBuffers(0, 1, &constantsBuffer);
    ID3D11ShaderResourceView* views[] { backgroundView_.Get(), pointerView_.Get() };
    context_->PSSetShaderResources(0, 2, views);
    constexpr float blendFactor[4] {};
    context_->OMSetBlendState(blendState_.Get(), blendFactor, 0xFFFFFFFFu);
    context_->OMSetDepthStencilState(depthStencilState_.Get(), 0);
    context_->OMSetRenderTargets(1, &outputView, nullptr);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullViews[] { nullptr, nullptr };
    ID3D11RenderTargetView* nullRenderTarget = nullptr;
    context_->PSSetShaderResources(0, 2, nullViews);
    context_->OMSetRenderTargets(1, &nullRenderTarget, nullptr);
    context_->VSSetShader(nullptr, nullptr, 0);
    context_->PSSetShader(nullptr, nullptr, 0);
    return true;
}

void DesktopPointerCompositor::reset() {
    pointerTexture_.Reset();
    pointerView_.Reset();
    backgroundTexture_.Reset();
    backgroundView_.Reset();
    backgroundWidth_ = 0;
    backgroundHeight_ = 0;
    pointerWidth_ = 0;
    pointerHeight_ = 0;
    pointerVisible_ = false;
    pointerPosition_ = {};
    shapeBytes_.clear();
}

}  // namespace clipture::capture
