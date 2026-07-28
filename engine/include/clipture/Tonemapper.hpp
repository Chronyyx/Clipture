#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace clipture {

class Tonemapper {
public:
    Tonemapper(Microsoft::WRL::ComPtr<ID3D11Device> device);
    ~Tonemapper();

    bool Initialize(std::string& errorMsg, float sdrWhiteLevel = 2.5f);
    
    // Tonemaps a 16-bit float input texture to an 8-bit output texture.
    // Both textures must be the same size.
    bool Process(
        Microsoft::WRL::ComPtr<ID3D11Texture2D> inputFloat16,
        Microsoft::WRL::ComPtr<ID3D11Texture2D> outputUnorm8,
        std::string& errorMsg
    );
    void ResetViewCache();

private:
    struct InputViewCacheEntry {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        D3D11_TEXTURE2D_DESC desc {};
    };

    struct OutputViewCacheEntry {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> view;
        D3D11_TEXTURE2D_DESC desc {};
    };

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> computeShader_;
    std::vector<InputViewCacheEntry> inputViews_;
    std::vector<OutputViewCacheEntry> outputViews_;
};

} // namespace clipture
