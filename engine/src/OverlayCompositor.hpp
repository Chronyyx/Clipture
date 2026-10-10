#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace clipture::capture {

// One captured overlay window (Discord overlay and similar click-through
// windows) placed in output pixels. Views are premultiplied-alpha BGRA8 and
// never written after capture.
struct OverlayLayer {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    int x = 0, y = 0;
    UINT width = 0, height = 0;
};

// Draws overlay layers bottom to top over a finished SDR frame, the way the
// compositor draws them over the game on screen.
class OverlayCompositor {
public:
    OverlayCompositor(
        Microsoft::WRL::ComPtr<ID3D11Device> device,
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context);

    bool initialize(std::string& error);
    bool composite(const std::vector<OverlayLayer>& layers, ID3D11RenderTargetView* output,
        UINT outputWidth, UINT outputHeight, std::string& error);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;
};

}  // namespace clipture::capture
