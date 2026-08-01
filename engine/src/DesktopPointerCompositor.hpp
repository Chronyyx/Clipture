#pragma once

#include "clipture/DesktopPointerShape.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace clipture::capture {

class DesktopPointerCompositor {
public:
    DesktopPointerCompositor(
        Microsoft::WRL::ComPtr<ID3D11Device> device,
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context);

    bool initialize(std::string& error);
    bool update(IDXGIOutputDuplication* duplication, const DXGI_OUTDUPL_FRAME_INFO& frameInfo, std::string& error);
    bool composite(
        ID3D11Texture2D* desktopTexture,
        ID3D11RenderTargetView* outputView,
        UINT outputWidth,
        UINT outputHeight,
        std::string& error);
    void reset();

private:
    struct Constants;

    bool uploadShape(const DecodedDesktopPointerShape& decoded, std::string& error);
    bool ensureBackgroundTexture(UINT width, UINT height, std::string& error);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pointerTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pointerView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backgroundTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> backgroundView_;
    UINT backgroundWidth_ = 0;
    UINT backgroundHeight_ = 0;
    UINT pointerWidth_ = 0;
    UINT pointerHeight_ = 0;
    POINT pointerPosition_ {};
    bool pointerVisible_ = false;
    std::vector<std::byte> shapeBytes_;
};

}  // namespace clipture::capture
