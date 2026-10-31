#include "clipture/GpuPriorityMemory.hpp"
#include <Windows.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

namespace {
std::string readAll(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}
}

int main() {
    using clipture::GpuPriorityMemory;
    const std::string key(64, 'a');
    const auto id = GpuPriorityMemory::gameId(key, "Overwatch.exe", "10de:2206:0:a1");
    require(id.size() == 64, "game id is not a SHA-256 hex digest");
    require(GpuPriorityMemory::gameId(key, "overwatch.EXE", "10de:2206:0:a1") == id, "game id depends on name case");
    require(GpuPriorityMemory::gameId(key, "Overwatch.exe", "10de:2684:0:a1") != id, "another GPU shares the game id");
    require(GpuPriorityMemory::gameId(std::string(64, 'b'), "Overwatch.exe", "10de:2206:0:a1") != id,
            "another install's key shares the game id");

    // Damaged files are not trusted; duplicate and malformed lines are dropped.
    require(!GpuPriorityMemory::parse("hello\n"), "a foreign file parsed");
    require(!GpuPriorityMemory::parse("clipture-gpu-priority v1\nkey 12\n"), "a short key parsed");
    const auto parsed = GpuPriorityMemory::parse(
        "clipture-gpu-priority v1\r\nkey " + key + "\r\n" + id + "\r\n" + id + "\r\nnot-an-id\r\n");
    require(parsed && parsed->keyHex == key && parsed->ids.size() == 1 && parsed->ids[0] == id,
            "file with CRLF, a duplicate and junk did not parse to one id");
    require(GpuPriorityMemory::parse(GpuPriorityMemory::format(*parsed))->ids == parsed->ids, "format does not round-trip");

    wchar_t temp[MAX_PATH] {};
    GetTempPathW(MAX_PATH, temp);
    const auto folder = std::filesystem::path(temp) / ("clipture-gpu-memory-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(folder);
    const auto file = folder / "gpu-priority-games.txt";
    {
        GpuPriorityMemory memory;
        memory.open(file.string(), "10de:2206:0:a1");
        require(memory.enabled() && memory.size() == 0 && !memory.contains("cs2.exe"), "a missing file is not empty");
        require(memory.remember("cs2.exe"), "remembering a game failed");
        require(!memory.remember("CS2.exe"), "the same game was remembered twice");
        require(memory.contains("cs2.exe") && !memory.contains("Overwatch.exe"), "lookup after remember is wrong");
    }
    const auto text = readAll(file);
    require(text.find("cs2") == std::string::npos, "the game name is stored in the file");
    require(!std::filesystem::exists(file.string() + ".tmp"), "the temporary file was left behind");
    {
        GpuPriorityMemory memory;
        memory.open(file.string(), "10de:2206:0:a1");
        require(memory.size() == 1 && memory.contains("cs2.exe"), "the game was not remembered across sessions");
        GpuPriorityMemory otherGpu;
        otherGpu.open(file.string(), "10de:2684:0:a1");
        require(!otherGpu.contains("cs2.exe"), "a new GPU inherited the memory");
    }
    {
        GpuPriorityMemory disabled;
        disabled.open("", "10de:2206:0:a1");
        require(!disabled.enabled() && !disabled.remember("cs2.exe"), "an empty path kept a memory");
    }
    std::filesystem::remove_all(folder);
    std::cout << "GPU priority memory: keyed ids, per-GPU entries, parsing, persistence and no stored names passed.\n";
    return 0;
}
