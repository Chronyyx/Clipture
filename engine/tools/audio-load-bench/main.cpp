// Audio replay CPU bench: drives the engine's real AudioReplayCoordinator (with
// the AAC and PCM-recovery replay stores) the way two live 48 kHz stereo
// sources do, a microphone and a game, one 10 ms packet each per 10 ms, and
// reports CPU per named thread from cycle counts. Engine-only; no capture,
// no audio device. Usage: clipture_audio_load_bench [seconds] [in-place|segments]
// in-place (as with "save in place" on) stores AAC in the in-place archive;
// segments uses the spill-segment files.
#include "clipture/AudioReplayCoordinator.hpp"
#include "clipture/MediaClock.hpp"
#include "clipture/PacketRingBuffer.hpp"
#include "clipture/ReplaySegmentStore.hpp"
#include "clipture/replay/InPlacePacketArchive.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace clipture;

namespace {

std::map<std::wstring, uint64_t> threadCycles() {
    std::map<std::wstring, uint64_t> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry {sizeof(entry)};
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        ULONG64 cycles = 0;
        QueryThreadCycleTime(thread, &cycles);
        PWSTR description = nullptr;
        std::wstring name = L"(unnamed)";
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description && *description) name = description;
        if (description) LocalFree(description);
        result[name] += cycles;
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return result;
}

double cyclesPerMs() {
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    const uint64_t c0 = __rdtsc();
    QueryPerformanceCounter(&start);
    Sleep(200);
    const uint64_t c1 = __rdtsc();
    QueryPerformanceCounter(&end);
    return static_cast<double>(c1 - c0) / ((end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart);
}

// One live source: a 10 ms packet of S16 stereo every 10 ms, published from
// its own thread as AudioCaptureWorker does.
void runSource(const char* sourceId, double toneHz, PacketRingBuffer& raw, AudioReplayCoordinator& coordinator,
               std::atomic<bool>& running, int64_t start100ns) {
    constexpr int kRate = 48000, kFrames = 480, kChannels = 2;
    uint32_t noise = 1;
    uint64_t index = 0;
    auto next = std::chrono::steady_clock::now();
    while (running.load()) {
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
        EncodedPacket packet;
        packet.kind = PacketKind::Audio;
        packet.codec = PacketCodec::PcmS16;
        packet.pts100ns = start100ns + static_cast<int64_t>(index) * 100'000;
        packet.dts100ns = packet.pts100ns;
        packet.duration100ns = 100'000;
        packet.sourceId = sourceId;
        packet.sampleRate = kRate;
        packet.channelCount = kChannels;
        packet.bitsPerSample = 16;
        packet.audioFrameCount = kFrames;
        packet.audible = true;
        packet.payload = raw.acquirePayload(kFrames * kChannels * sizeof(int16_t));
        auto* samples = reinterpret_cast<int16_t*>(mutablePayload(packet).data());
        for (int i = 0; i < kFrames; ++i) {
            noise = noise * 1664525u + 1013904223u;
            const double t = static_cast<double>(index * kFrames + i) / kRate;
            const auto value = static_cast<int16_t>(4000 * std::sin(2 * 3.14159265 * toneHz * t) + ((noise >> 20) & 511) - 256);
            samples[2 * i] = samples[2 * i + 1] = value;
        }
        ++index;
        coordinator.publish(std::move(packet));
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 30;
    const bool inPlace = argc <= 2 || std::strcmp(argv[2], "segments") != 0;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "clipture-audio-load-bench";
    std::filesystem::create_directories(root);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    // The engine's sizes (Engine.cpp): 5 min rings, 4 MiB payload pools.
    PacketRingBuffer raw(5LL * 60LL * 10'000'000LL, 4u * 1024u * 1024u);
    PacketRingBuffer aac(5LL * 60LL * 10'000'000LL, 4u * 1024u * 1024u);
    auto storeOptions = [&](const char* name) {
        ReplaySegmentStoreOptions options;
        options.streamName = name;
        options.rootDirectory = root;
        options.targetSegmentDuration100ns = 30LL * 10'000'000LL;
        options.targetSegmentBytes = 8u * 1024u * 1024u;
        return options;
    };
    ReplaySegmentStore aacStore(storeOptions("audio"));
    ReplaySegmentStore pcmStore(storeOptions("pcm-recovery"));
    aacStore.start();
    pcmStore.start();
    // As Engine::configure does with "save in place" on (no resident budget).
    auto archive = std::make_shared<replay::InPlacePacketArchive>();
    if (inPlace) {
        archive->configure(root / "save-folder", true, 64ull * 1024 * 1024);
        aacStore.setInPlaceArchive(archive);
        aacStore.setResidentPayloadBudget(0);
    }
    AudioReplayCoordinator coordinator(raw, aac, &aacStore, &pcmStore);
    coordinator.updateRouting({{"microphone-pcm", "microphone-pcm"}, {"app:game.exe", "game"}});
    coordinator.start();

    const double perMs = cyclesPerMs();
    std::atomic<bool> running {true};
    const int64_t start100ns = monotonicNow100ns();
    std::thread mic([&] { runSource("microphone-pcm", 220.0, raw, coordinator, running, start100ns); });
    std::thread game([&] { runSource("app:game.exe", 440.0, raw, coordinator, running, start100ns); });
    // Warm up, then measure.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const auto before = threadCycles();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const auto after = threadCycles();
    running = false;
    mic.join();
    game.join();
    const auto stats = coordinator.stats();
    coordinator.stop();
    aacStore.stop();
    pcmStore.stop();

    double total = 0;
    std::printf("thread CPU, ms per second (%d s, %s):\n", seconds, inPlace ? "in-place archive" : "segments");
    for (const auto& [name, cycles] : after) {
        const auto found = before.find(name);
        const uint64_t delta = cycles - (found == before.end() ? 0 : found->second);
        const double msPerSecond = delta / perMs / seconds;
        total += msPerSecond;
        if (msPerSecond >= 0.05) std::printf("  %-24ls %8.2f\n", name.c_str(), msPerSecond);
    }
    std::printf("  %-24s %8.2f\nqueue overflows %llu, encoder restarts %llu, raw ring %zu packets\n", "total", total,
                static_cast<unsigned long long>(stats.queueOverflows), static_cast<unsigned long long>(stats.encoderRestarts),
                raw.size());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
