#pragma once

#include <windows.h>
#include <mmsystem.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace clipture::stress {

// Looping soft noise played by this process while GPU load is active, so
// per-process game-audio capture sees a source that turns loud with the effect.
class AudioLoad {
public:
    explicit AudioLoad(bool enabled) : enabled_(enabled) {
        if (!enabled_) return;
        constexpr uint32_t rate = 48'000, channels = 2, seconds = 2;
        const uint32_t samples = rate * seconds;
        const uint32_t dataBytes = samples * channels * sizeof(int16_t);
        wav_.resize(44 + dataBytes);
        auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav_[at], &v, 4); };
        auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav_[at], &v, 2); };
        std::memcpy(&wav_[0], "RIFF", 4); put32(4, 36 + dataBytes); std::memcpy(&wav_[8], "WAVEfmt ", 8);
        put32(16, 16); put16(20, 1); put16(22, channels); put32(24, rate);
        put32(28, rate * channels * 2); put16(32, channels * 2); put16(34, 16);
        std::memcpy(&wav_[36], "data", 4); put32(40, dataBytes);
        // Quiet, low-passed noise (a soft hiss) with slow swells; no clicks.
        uint32_t seed = 12345;
        double smooth = 0;
        auto* pcm = reinterpret_cast<int16_t*>(&wav_[44]);
        for (uint32_t i = 0; i < samples; ++i) {
            seed = seed * 1664525u + 1013904223u;
            smooth += 0.05 * (static_cast<int32_t>(seed) / 2147483648.0 - smooth);
            const double swell = 0.6 + 0.4 * std::sin(i * 2.0 * 3.14159265 / samples);
            pcm[i * 2] = pcm[i * 2 + 1] = static_cast<int16_t>(smooth * swell * 32767 * 0.5);
        }
    }
    ~AudioLoad() { setActive(false); }

    void setActive(bool active) {
        if (!enabled_ || active == playing_) return;
        playing_ = active;
        if (active) PlaySoundW(reinterpret_cast<LPCWSTR>(wav_.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP);
        else PlaySoundW(nullptr, nullptr, 0);
    }

private:
    bool enabled_;
    bool playing_ = false;
    std::vector<uint8_t> wav_;
};

}  // namespace clipture::stress
