#include "clipture/PcmBlockMixer.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void subSampleOffsetsDoNotDoubleSamples() {
    // Start aligned, then introduce a sub-sample phase offset as a clock
    // correction would do later in a session. Old signed truncation mixed the
    // preceding packet's last sample over the next packet's first sample.
    for (const int rate : {8000, 44100, 48000, 96000}) {
        const int frames = rate / 100;
        std::vector<int16_t> packet(frames * 2, 20000);
        std::vector<float> mixed(frames * 2);
        for (int64_t phase = -10'000'000 / rate; phase <= 10'000'000 / rate; ++phase) {
            std::fill(mixed.begin(), mixed.end(), 0.0f);
            for (int offset = -1; offset <= 1; ++offset) {
                clipture::addPcmToMixBlock(packet, 2, rate, offset * 100000LL + phase, mixed);
            }
            for (float sample : mixed) {
                require(sample == 20000, "sub-sample clock phase doubled or dropped PCM at block boundary");
            }
        }
    }
}

void separateSourcesStillSum() {
    std::array<int16_t, 4> first {100, -100, 200, -200};
    std::array<int16_t, 4> second {10, -10, 20, -20};
    std::array<float, 4> output {};
    clipture::addPcmToMixBlock(first, 2, 48000, 0, output);
    clipture::addPcmToMixBlock(second, 2, 48000, 0, output);
    require(output == std::array<float, 4>{110, -110, 220, -220}, "distinct sources must still mix");
}
}

int main() {
    try {
        subSampleOffsetsDoNotDoubleSamples();
        separateSourcesStillSum();
        std::cout << "PCM block mixer tests passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
