#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace clipture {
// The caller reserves an unmapped NVENC output slot before constructing its
// view. This creates metadata only, not a new pixel buffer or a GPU wait.
inline HRESULT nv12ConversionTarget(ID3D11VideoDevice* device,
    ID3D11VideoProcessorEnumerator* enumerator, ID3D11Texture2D* texture,
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>& view) {
    if (!device || !enumerator || !texture) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12 || !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) return E_INVALIDARG;
    if (view) return S_OK;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output {};
    output.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    return device->CreateVideoProcessorOutputView(texture, enumerator, &output, &view);
}
}
