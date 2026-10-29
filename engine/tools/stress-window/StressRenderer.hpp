#pragma once

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace clipture::stress {

struct RenderedFrame {
    bool presented = false;
    // GPU time of the load layers, when a timestamp measurement was requested.
    double loadGpuMs = -1;
    // DXGI_FRAME_PRESENTATION_MODE of the last statistics (0 composed, 1 overlay,
    // 2 none, 3 composition failure), or -1 when unavailable.
    int presentationMode = -1;
};

// Uncapped flip-model renderer: scrolling detail plus a moving bar so every
// frame differs, optional alpha-blended ALU-heavy layers for GPU pressure, and
// the frame counter stamp drawn last.
class StressRenderer {
public:
    // `hdr` presents FP16 scRGB with highlights above SDR white instead of 8-bit SDR.
    // `computeLoad` issues load layers as compute dispatches instead of draws.
    bool initialize(HWND window, int width, int height, int maxFrameLatency, bool hdr, bool computeLoad,
                    std::string& error);
    // D3D11 GPU thread priority (-7..7), as games may raise theirs. Returns the active value.
    int setGpuPriority(int priority);
    RenderedFrame render(uint32_t counter, double seconds, int loadLayers, bool measureLoad);
    // Draws six known-colour squares for capture colour checks (SDR only).
    void showPatches(bool show) { patches_ = show && !hdr_; }

    // GPU milliseconds of one load layer, averaged over `frames` measured frames.
    double calibrateLayerMs(int layers, int frames);

private:
    bool compileShaders(std::string& error);
    double readLoadTimestamps();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
public:
    ID3D11Device* device() const { return device_.Get(); }
private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> sceneShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> loadShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> stampShader_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> loadComputeShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> compositeShader_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> density_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> densityUav_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> densitySrv_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> alphaBlend_;
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint_;
    Microsoft::WRL::ComPtr<ID3D11Query> loadBegin_;
    Microsoft::WRL::ComPtr<ID3D11Query> loadEnd_;
    int width_ = 0;
    int height_ = 0;
    bool tearing_ = false;
    bool hdr_ = false;
    bool computeLoad_ = false;
    bool patches_ = false;
};

}  // namespace clipture::stress
