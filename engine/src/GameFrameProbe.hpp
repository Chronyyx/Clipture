#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace clipture::capture {

// Whether a captured game-window frame shows anything. Some games hide their
// window from capture or render outside it, which window capture returns as
// black. Reads back five small patches (centre and quadrants); `staging` is
// reused between calls. Waits for the GPU, so call it rarely.
bool gameFrameHasContent(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame,
    UINT width, UINT height, Microsoft::WRL::ComPtr<ID3D11Texture2D>& staging);

}  // namespace clipture::capture
