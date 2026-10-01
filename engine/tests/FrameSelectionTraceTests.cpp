#include "clipture/FrameSelectionTrace.hpp"
#include <Windows.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

void require(bool ok) { if (!ok) std::abort(); }
int main() {
    wchar_t temp[MAX_PATH]; require(GetTempPathW(MAX_PATH, temp) != 0);
    const auto dir = std::filesystem::path(temp) / (L"clipture-selection-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    require(std::filesystem::create_directory(dir));
    clipture::FrameSelectionTrace disabled(clipture::FrameSelectionTrace::Config{});
    require(!disabled.enabled());
    const auto file = dir / L"trace.csv";
    {
        clipture::FrameSelectionTrace trace({file.wstring(), 20, 2, 8});
        require(trace.enabled()); trace.record({'p'}); trace.record({'t'}); trace.record({'p'});
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        require(!trace.enabled());
    }
    std::ifstream input(file); const std::string bytes((std::istreambuf_iterator<char>(input)), {}); input.close();
    require(bytes.find("\"lost\":1") != std::string::npos && bytes.find("\"complete\":true") != std::string::npos);
    { clipture::FrameSelectionTrace refused({file.wstring(), 10, 2, 8}); require(!refused.enabled()); }
    { clipture::FrameSelectionTrace partial({(dir / L"partial.csv").wstring(), 10000, 2, 8}); partial.record({'p'}); }
    std::ifstream partial(dir / L"partial.csv"); const std::string partialBytes((std::istreambuf_iterator<char>(partial)), {}); partial.close();
    require(partialBytes.find("\"complete\":false") != std::string::npos);
    std::filesystem::remove(file); std::filesystem::remove(dir / L"partial.csv"); std::filesystem::remove(dir);
    std::cout << "Trace disabled mode, bounded loss, timed completion, no overwrite, and partial shutdown passed.\n";
}
