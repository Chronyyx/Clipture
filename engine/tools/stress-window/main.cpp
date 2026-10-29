// Synthetic capture workload: an uncapped, frame-stamped, full-screen window
// whose GPU load follows a scripted timeline. See README.md.
#include "Reflex.hpp"
#include "AudioLoad.hpp"
#include "CpuLoad.hpp"
#include "StressRenderer.hpp"
#include "StressTimeline.hpp"
#include "VramLoad.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace clipture::stress;

namespace {

struct Options {
    std::string timeline = kDefaultTimeline;
    std::filesystem::path out = ".";
    double layerMs = 0;
    int maxFrameLatency = 1;
    bool autostart = false;
    bool hdr = false;
    int renderWidth = 0;
    int renderHeight = 0;
    int cpuThreads = 0;
    int cpuBusyPercent = 100;
    bool computeLoad = false;
    bool audio = false;
    bool cpuGamesPriority = false;
    int gpuPriority = 0;
    int vramMb = 0;
    bool reflex = false;
};

struct FrameRecord {
    uint32_t counter;
    int phase;
    int64_t presentUs;
    bool foreground;
};

std::atomic<bool> g_start{false};
std::atomic<bool> g_stop{false};

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) g_stop = true;
        return 0;
    case WM_SETCURSOR:
        SetCursor(nullptr);  // keep the pointer off the stamp
        return TRUE;
    case WM_CLOSE:
        g_stop = true;
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

// A game being played owns the foreground, and Windows schedules the foreground
// process differently, so the window must hold it. Windows refuses focus
// changes from background processes unless input is attached to the current
// foreground thread; a synthetic Alt tap is the last resort.
bool takeForeground(HWND window) {
    for (int attempt = 0; attempt < 2 && GetForegroundWindow() != window; ++attempt) {
        const DWORD self = GetCurrentThreadId();
        const DWORD owner = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const bool attached = owner && owner != self && AttachThreadInput(self, owner, TRUE);
        if (attempt == 1) {
            INPUT alt[2]{};
            alt[0].type = alt[1].type = INPUT_KEYBOARD;
            alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
            alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, alt, sizeof(INPUT));
        }
        BringWindowToTop(window);
        SetForegroundWindow(window);
        SetFocus(window);
        if (attached) AttachThreadInput(self, owner, FALSE);
    }
    return GetForegroundWindow() == window;
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--timeline" && hasValue) options.timeline = argv[++i];
        else if (arg == "--out" && hasValue) options.out = argv[++i];
        else if (arg == "--layer-ms" && hasValue) options.layerMs = std::atof(argv[++i]);
        else if (arg == "--max-latency" && hasValue) options.maxFrameLatency = std::clamp(std::atoi(argv[++i]), 1, 16);
        else if (arg == "--autostart") options.autostart = true;
        else if (arg == "--hdr") options.hdr = true;
        else if (arg == "--reflex") options.reflex = true;
        else if (arg == "--compute-load") options.computeLoad = true;
        else if (arg == "--audio") options.audio = true;
        else if (arg == "--cpu-games-priority") options.cpuGamesPriority = true;
        else if (arg == "--gpu-priority" && hasValue) options.gpuPriority = std::clamp(std::atoi(argv[++i]), -7, 7);
        else if (arg == "--cpu-threads" && hasValue) options.cpuThreads = std::clamp(std::atoi(argv[++i]), 0, 256);
        else if (arg == "--vram-mb" && hasValue) options.vramMb = std::max(0, std::atoi(argv[++i]));
        else if (arg == "--cpu-busy" && hasValue) options.cpuBusyPercent = std::clamp(std::atoi(argv[++i]), 1, 100);
        else if (arg == "--render-size" && hasValue &&
                 std::sscanf(argv[++i], "%dx%d", &options.renderWidth, &options.renderHeight) == 2) {}
        else {
            std::cerr << "Usage: clipture_stress_window [--timeline name:seconds:loadMs,...] [--out dir]\n"
                         "       [--layer-ms ms] [--max-latency 1-16] [--autostart] [--hdr] [--render-size WxH]\n"
                         "       [--cpu-threads N] [--cpu-busy 1-100] [--compute-load] [--audio] [--cpu-games-priority] [--gpu-priority -7..7]\n"
                         "       [--vram-mb N]\n";
            return false;
        }
    }
    return true;
}

// Control lines on stdin: "start" begins the timeline; "stop" or EOF exits.
void readControl() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "start") g_start = true;
        if (line == "stop") break;
    }
    g_stop = true;
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(fraction * (values.size() - 1))];
}

void writeResults(const Options& options, const StressTimeline& timeline, const std::vector<FrameRecord>& frames,
                  const std::vector<int>& layers, int width, int height, int bufferWidth, int bufferHeight) {
    std::filesystem::create_directories(options.out);
    std::ofstream csv(options.out / "frames.csv");
    csv << "frame,phase,presentUs\n";
    for (const auto& f : frames) csv << f.counter << ',' << f.phase << ',' << f.presentUs << '\n';

    std::vector<std::string> names{"lead"};
    for (const auto& phase : timeline.phases()) names.push_back(phase.name);
    names.push_back("tail");
    std::ofstream json(options.out / "summary.json");
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const int refreshHz = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) ? static_cast<int>(mode.dmDisplayFrequency) : 0;
    json << "{\"width\":" << width << ",\"height\":" << height << ",\"refreshHz\":" << refreshHz << ",\"bufferWidth\":" << bufferWidth
         << ",\"bufferHeight\":" << bufferHeight << ",\"layerMs\":" << options.layerMs
         << ",\"maxFrameLatency\":" << options.maxFrameLatency << ",\"reflex\":" << (options.reflex ? "true" : "false")
         << ",\"hdr\":" << (options.hdr ? "true" : "false") << ",\"timeline\":\"" << options.timeline
         << "\",\"phases\":[";
    for (size_t phase = 0; phase < names.size(); ++phase) {
        std::vector<double> intervals;
        int64_t first = -1, last = -1, count = 0, foreground = 0;
        uint32_t firstFrame = 0, lastFrame = 0;
        for (size_t i = 0; i < frames.size(); ++i) {
            if (frames[i].phase != static_cast<int>(phase)) continue;
            if (first < 0) { first = frames[i].presentUs; firstFrame = frames[i].counter; }
            if (count > 0) intervals.push_back((frames[i].presentUs - last) / 1000.0);
            last = frames[i].presentUs;
            lastFrame = frames[i].counter;
            foreground += frames[i].foreground ? 1 : 0;
            ++count;
        }
        const double seconds = count > 1 ? (last - first) / 1e6 : 0;
        json << (phase ? "," : "") << "{\"index\":" << phase << ",\"name\":\"" << names[phase]
             << "\",\"layers\":" << layers[phase] << ",\"frames\":" << count << ",\"firstFrame\":" << firstFrame
             << ",\"lastFrame\":" << lastFrame << ",\"foregroundPct\":" << (count ? 100.0 * foreground / count : 0) << ",\"seconds\":" << seconds
             << ",\"fps\":" << (seconds > 0 ? (count - 1) / seconds : 0)
             << ",\"frameMsP50\":" << percentile(intervals, 0.5) << ",\"frameMsP99\":" << percentile(intervals, 0.99)
             << ",\"frameMsMax\":" << percentile(intervals, 1.0) << "}";
    }
    json << "]}\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) return 2;
    std::string error;
    const auto timeline = StressTimeline::parse(options.timeline, error);
    if (!timeline) {
        std::cerr << error << '\n';
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT bounds = monitor.rcMonitor;
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"CliptureStressWindow";
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName, L"Clipture stress window", WS_POPUP,
                                  bounds.left, bounds.top, width, height, nullptr, nullptr,
                                  windowClass.hInstance, nullptr);
    ShowWindow(window, SW_SHOW);
    takeForeground(window);

    StressRenderer renderer;
    const int bufferWidth = options.renderWidth > 0 ? std::min(options.renderWidth, width) : width;
    const int bufferHeight = options.renderHeight > 0 ? std::min(options.renderHeight, height) : height;
    if (!renderer.initialize(window, bufferWidth, bufferHeight, options.maxFrameLatency, options.hdr,
                             options.computeLoad, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    Reflex reflex;
    if (options.reflex && !reflex.enable(renderer.device(), true, error)) {
        std::cerr << "Reflex: " << error << '\n';
        return 1;
    }
    VramLoad vramLoad(options.vramMb);
    const int activeGpuPriority = options.gpuPriority ? renderer.setGpuPriority(options.gpuPriority) : 0;
    if (options.layerMs <= 0) options.layerMs = renderer.calibrateLayerMs(8, 40);
    if (options.layerMs <= 0) {
        std::cerr << "GPU timestamp calibration failed; pass --layer-ms\n";
        return 1;
    }
    std::vector<int> layers{0};
    for (const auto& phase : timeline->phases()) {
        layers.push_back(std::clamp(static_cast<int>(std::lround(phase.loadMs / options.layerMs)), 0, 4000));
    }
    layers.push_back(0);
    std::printf("{\"event\":\"ready\",\"layerMs\":%.6f,\"width\":%d,\"height\":%d,\"gpuPriority\":%d,\"vramMb\":%d}\n",
                options.layerMs, width, height, activeGpuPriority, vramLoad.heldMegabytes());
    std::fflush(stdout);

    if (options.autostart) g_start = true;
    else std::thread(readControl).detach();

    LARGE_INTEGER frequency, origin, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&origin);
    auto micros = [&](const LARGE_INTEGER& t) { return (t.QuadPart - origin.QuadPart) * 1'000'000 / frequency.QuadPart; };
    std::vector<FrameRecord> frames;
    frames.reserve(1 << 20);
    // CPU load runs only in phases with GPU load, like a game's smoke simulation.
    CpuLoad cpuLoad(options.cpuThreads, options.cpuBusyPercent, options.cpuGamesPriority);
    AudioLoad audioLoad(options.audio);
    int64_t startedUs = -1;
    bool finishedReported = false;
    for (uint32_t counter = 1; !g_stop; ++counter) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        QueryPerformanceCounter(&now);
        const int64_t nowUs = micros(now);
        if (g_start && startedUs < 0) {
            startedUs = nowUs;
            takeForeground(window);
            std::printf("{\"event\":\"started\"}\n");
            std::fflush(stdout);
        }
        int phase = 0;
        if (startedUs >= 0) {
            const int index = timeline->phaseAt((nowUs - startedUs) / 1e6);
            phase = index < 0 ? static_cast<int>(layers.size()) - 1 : index + 1;
        }
        if (phase == static_cast<int>(layers.size()) - 1 && !finishedReported) {
            finishedReported = true;
            std::printf("{\"event\":\"finished\",\"residentVramMb\":%d}\n", vramLoad.residentMegabytes());
            std::fflush(stdout);
            if (options.autostart) break;
        }
        cpuLoad.setActive(layers[phase] > 0);
        audioLoad.setActive(layers[phase] > 0);
        vramLoad.touch();
        reflex.sleep();
        renderer.render(counter, nowUs / 1e6, layers[phase], false);
        QueryPerformanceCounter(&now);
        frames.push_back({counter, phase, micros(now), GetForegroundWindow() == window});
    }
    DestroyWindow(window);
    writeResults(options, *timeline, frames, layers, width, height, bufferWidth, bufferHeight);
    return 0;
}
