#pragma once
#include "SharedTextureReader.hpp"

namespace clipture {
// Submit-thread-owned scratch, not an input queue. Subsequent conversion reads
// and the next overwrite are ordered on the same encoder immediate context.
class EncoderSourceSnapshot {
public:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copyAndRetire(
        ID3D11Device* device, ID3D11DeviceContext* context,
        SharedTextureReader& reader, ID3D11Texture2D* source,
        const std::shared_ptr<GpuTextureReadState>& lifetime, std::string& error);
private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    GpuStageProbe copyProbe_ {"source-private-copy"};
};
}
