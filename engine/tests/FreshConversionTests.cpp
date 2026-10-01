#include "clipture/FreshConversionPolicy.hpp"
#include "Nv12ConversionTarget.hpp"
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;
static void require(bool ok) { if (!ok) std::abort(); }

static std::vector<unsigned char> readPixels(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &readback)));
    context->CopyResource(readback.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    require(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    std::vector<unsigned char> bytes(desc.Width * desc.Height * 3 / 2);
    for (UINT row = 0; row < desc.Height * 3 / 2; ++row) {
        std::memcpy(bytes.data() + row * desc.Width,
            static_cast<unsigned char*>(mapped.pData) + row * mapped.RowPitch, desc.Width);
    }
    context->Unmap(readback.Get(), 0);
    return bytes;
}

int main(int argc, char**) {
    clipture::FreshConversionPolicy policy;
    require(!policy.direct(false, 1, 1) && !policy.direct(true, 1, 0));
    require(policy.direct(true, 1, 1));
    policy.converted(1, 1);
    for (int repeat = 0; repeat < 64; ++repeat) require(!policy.direct(true, 1, 1));
    require(policy.direct(true, 1, 2) && policy.direct(true, 2, 1));
    policy = {};
    require(policy.direct(true, 1, 1));
    if (argc == 1) { std::cout << "Fresh conversion routing, repeat storms, epoch/reset passed.\n"; return 0; }

    // No desktop capture. Compare the real VideoProcessorBlt's NV12 planes
    // through the control (canonical+copy) and candidate (direct-slot) targets.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 77;
    ComPtr<ID3D11VideoDevice> video;
    ComPtr<ID3D11VideoContext> vc;
    if (FAILED(device.As(&video)) || FAILED(context.As(&vc))) return 77;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = content.InputHeight = content.OutputWidth = content.OutputHeight = 32;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    if (FAILED(video->CreateVideoProcessorEnumerator(&content, &enumerator)) ||
        FAILED(video->CreateVideoProcessor(enumerator.Get(), 0, &processor))) return 77;
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = desc.Height = 32; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    ComPtr<ID3D11Texture2D> source, canonical, control, candidate;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &source)));
    desc.Format = DXGI_FORMAT_NV12;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &canonical)));
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &control)));
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &candidate)));
    ComPtr<ID3D11VideoProcessorOutputView> canonicalView, candidateView, invalidView;
    require(FAILED(clipture::nv12ConversionTarget(video.Get(), enumerator.Get(), source.Get(), invalidView)));
    require(SUCCEEDED(clipture::nv12ConversionTarget(video.Get(), enumerator.Get(), canonical.Get(), canonicalView)));
    require(SUCCEEDED(clipture::nv12ConversionTarget(video.Get(), enumerator.Get(), candidate.Get(), candidateView)));
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc {};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> input;
    require(SUCCEEDED(video->CreateVideoProcessorInputView(source.Get(), enumerator.Get(), &inputDesc, &input)));
    D3D11_VIDEO_PROCESSOR_STREAM stream {};
    stream.Enable = TRUE; stream.pInputSurface = input.Get();
    std::array<uint32_t, 32 * 32> pixels;
    for (uint32_t frame = 0; frame < 12; ++frame) {
        for (uint32_t i = 0; i < pixels.size(); ++i) pixels[i] = 0xff000000 | ((i * 71237 + frame * 54321) & 0xffffff);
        context->UpdateSubresource(source.Get(), 0, nullptr, pixels.data(), 32 * 4, 0);
        require(SUCCEEDED(vc->VideoProcessorBlt(processor.Get(), canonicalView.Get(), 0, 1, &stream)));
        context->CopyResource(control.Get(), canonical.Get());
        require(SUCCEEDED(vc->VideoProcessorBlt(processor.Get(), candidateView.Get(), 0, 1, &stream)));
        require(readPixels(device.Get(), context.Get(), control.Get()) == readPixels(device.Get(), context.Get(), candidate.Get()));
    }
    std::cout << "Direct NV12 output matches canonical+copy on both planes across reused slots.\n";
}
