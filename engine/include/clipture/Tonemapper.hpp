#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <memory>

namespace clipture {
class GpuStageProbe;

class Tonemapper {
public:
    Tonemapper(Microsoft::WRL::ComPtr<ID3D11Device> device);
    ~Tonemapper();

    bool Initialize(std::string& errorMsg, float sdrWhiteLevel = 2.5f);
    
    // Tonemaps a 16-bit float input view to an 8-bit output view of the same
    // size. The caller owns the views, so freeing a texture is never delayed
    // by this class: a view keeps its texture (and the driver's system-memory
    // backing for it) alive.
    bool Process(
        ID3D11ShaderResourceView* inputFloat16,
        ID3D11UnorderedAccessView* outputUnorm8,
        UINT width,
        UINT height,
        std::string& errorMsg
    );
    // Convenience for callers without long-lived views; creates them per call.
    bool Process(
        Microsoft::WRL::ComPtr<ID3D11Texture2D> inputFloat16,
        Microsoft::WRL::ComPtr<ID3D11Texture2D> outputUnorm8,
        std::string& errorMsg
    );

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> computeShader_;
    std::unique_ptr<GpuStageProbe> timingProbe_;
};

} // namespace clipture
