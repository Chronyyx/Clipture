// Capture floor: a Windows.Graphics.Capture session that does nothing with its
// frames. Run against the stress window, it measures what capture alone costs
// the game (forced DWM composition and WGC delivery) with no copy, tone map or
// encode, the floor under any capture pipeline built on WGC.
// Usage: clipture_capture_floor --process <exe> | --monitor  [--seconds N]
//        [--fp16] [--interval-ms N]. Ends on N seconds or stdin EOF.
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <tlhelp32.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

namespace {
struct Target { DWORD pid = 0; HWND window = nullptr; };

BOOL CALLBACK findWindow(HWND window, LPARAM parameter) {
    auto* target = reinterpret_cast<Target*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == target->pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
        target->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND windowOfProcess(const wchar_t* exe) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry {sizeof(entry)};
    Target target;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, exe) == 0) { target.pid = entry.th32ProcessID; break; }
    }
    CloseHandle(snapshot);
    if (target.pid) EnumWindows(findWindow, reinterpret_cast<LPARAM>(&target));
    return target.window;
}
}  // namespace

int main(int argc, char** argv) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::wstring process;
    bool monitor = false, fp16 = false;
    int seconds = 600, intervalMs = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--process" && i + 1 < argc) { const std::string v = argv[++i]; process.assign(v.begin(), v.end()); }
        else if (arg == "--monitor") monitor = true;
        else if (arg == "--fp16") fp16 = true;
        else if (arg == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (arg == "--interval-ms" && i + 1 < argc) intervalMs = std::atoi(argv[++i]);
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
        std::cerr << "D3D11 device failed\n";
        return 1;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    device.As(&dxgi);
    winrt::com_ptr<IInspectable> inspectable;
    CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put());
    auto direct3d = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item {nullptr};
    if (monitor) {
        const POINT origin {0, 0};
        HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        winrt::check_hresult(interop->CreateForMonitor(primary, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
    } else {
        HWND window = nullptr;
        for (int attempt = 0; attempt < 100 && !window; ++attempt) {
            window = windowOfProcess(process.c_str());
            if (!window) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!window) { std::cerr << "window not found\n"; return 1; }
        winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
    }
    const auto format = fp16 ? DirectXPixelFormat::R16G16B16A16Float : DirectXPixelFormat::B8G8R8A8UIntNormalized;
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(direct3d, format, 3, item.Size());
    std::atomic<uint64_t> frames {0};
    pool.FrameArrived([&](Direct3D11CaptureFramePool const& sender, auto const&) {
        while (auto frame = sender.TryGetNextFrame()) { frame.Close(); ++frames; }
    });
    auto session = pool.CreateCaptureSession(item);
    try { session.IsBorderRequired(false); } catch (...) {}
    if (intervalMs > 0) {
        if (auto cadence = session.try_as<IGraphicsCaptureSession5>()) {
            cadence.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan {int64_t {intervalMs} * 10'000});
        }
    }
    session.StartCapture();
    std::cout << "{\"event\":\"capturing\"}" << std::endl;
    std::atomic<bool> stop {false};
    std::thread input([&] { std::string line; while (std::getline(std::cin, line)) {} stop = true; });
    input.detach();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!stop && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    session.Close();
    pool.Close();
    std::cout << "{\"event\":\"done\",\"frames\":" << frames.load() << "}" << std::endl;
    return 0;
}
