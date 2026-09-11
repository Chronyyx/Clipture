#include "clipture/AudioReplayCoordinator.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
std::vector<clipture::EncodedPacket> encodeSession(int64_t phase) {
    constexpr int rate = 48000;
    constexpr int frames = 480;
    constexpr int blocks = 18000; // Three minutes, advanced without wall-clock sleeps.
    constexpr int64_t origin = 134000000000000000LL; // Realistic FILETIME magnitude.
    clipture::PacketRingBuffer raw(240LL * 10'000'000);
    clipture::PacketRingBuffer aac(240LL * 10'000'000);
    clipture::AudioReplayCoordinator coordinator(raw, aac);
    coordinator.updateRouting({{"app:fixture.exe", "app:fixture.exe"}});
    coordinator.start();
    for (int block = 0; block < blocks; ++block) {
        clipture::EncodedPacket packet;
        packet.codec = clipture::PacketCodec::PcmS16;
        // Restore the exact endpoint for the final second so the shutdown
        // drain's complete-block policy is not part of this waveform test.
        packet.pts100ns = origin + block * 100000LL + (block >= 6000 && block < blocks - 100 ? phase : 0);
        packet.duration100ns = 100000;
        packet.sourceId = "app:fixture.exe";
        packet.sampleRate = rate;
        packet.channelCount = 2;
        packet.bitsPerSample = 16;
        packet.payload = raw.acquirePayload(frames * 2 * sizeof(int16_t));
        auto* samples = reinterpret_cast<int16_t*>(packet.payload->data());
        for (int frame = 0; frame < frames; ++frame) {
            const double angle = 6.283185307179586 * 997 * (block * frames + frame) / rate;
            samples[2 * frame] = static_cast<int16_t>(20000 * std::cos(angle));
            samples[2 * frame + 1] = static_cast<int16_t>(18000 * std::sin(angle));
        }
        coordinator.publish(std::move(packet));
        if (block % 100 == 99 && !coordinator.waitUntil(
                origin + block * 100000LL - 3'000'000,
                std::chrono::seconds(10))) {
            throw std::runtime_error("audio fixture failed to consume its input batch");
        }
    }
    coordinator.stop();
    const auto stats = coordinator.stats();
    if (stats.queueOverflows || stats.encoderRestarts) throw std::runtime_error("audio fixture overflowed or restarted");
    return aac.snapshot();
}
}

bool testAudioMixerClockPhaseDoesNotChangeAac() {
    try {
        const auto baseline = encodeSession(0);
        if (baseline.size() < 8000) throw std::runtime_error("three-minute fixture output is incomplete");
        for (const int64_t phase : {1LL, -1LL}) {
            const auto shifted = encodeSession(phase);
            if (shifted.size() != baseline.size()) throw std::runtime_error("clock phase changed AAC packet count");
            for (std::size_t i = 0; i < baseline.size(); ++i) {
                if (baseline[i].pts100ns != shifted[i].pts100ns ||
                    *baseline[i].payload != *shifted[i].payload) {
                    throw std::runtime_error("sub-sample phase after one minute changed encoded audio");
                }
            }
        }
        std::cout << "Three-minute mixer/AAC phase parity passed (0, +100 ns, -100 ns).\n";
        return true;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return false;
    }
}
