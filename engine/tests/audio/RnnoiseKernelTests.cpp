// RNNoise as the microphone path drives it: 480-sample frames of S16-scale
// mono at 48 kHz. Checks the output is finite and that noise alone is
// attenuated while a voiced signal passes, and reports the cost per frame.
// `--dump <file>` writes every output sample and VAD value (float32) so two
// builds can be compared sample for sample.
#include "rnnoise.h"
extern "C" {
#include "rnn.h"
#include "rnnoise_data.h"
#include "denoise.h"
}

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

// Deterministic input: 10 s of noise only, then 10 s of a voiced tone
// (harmonics of 140 Hz, syllable-rate envelope) over the same noise.
std::vector<float> testSignal(int sampleRate, int seconds) {
    std::vector<float> samples(static_cast<std::size_t>(sampleRate) * seconds);
    uint32_t state = 12345;
    const double pi = 3.14159265358979323846;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(state >> 8) / 16777216.0 - 0.5) * 1200.0;
        double voice = 0;
        if (i >= samples.size() / 2) {
            const double t = static_cast<double>(i) / sampleRate;
            const double envelope = 0.5 + 0.5 * std::sin(2 * pi * 4.0 * t);
            for (int harmonic = 1; harmonic <= 12; ++harmonic) {
                voice += std::sin(2 * pi * 140.0 * harmonic * t) / harmonic;
            }
            voice *= 5000.0 * envelope;
        }
        samples[i] = static_cast<float>(noise + voice);
    }
    return samples;
}

double energy(const std::vector<float>& samples, std::size_t from, std::size_t to) {
    double sum = 0;
    for (std::size_t i = from; i < to; ++i) sum += static_cast<double>(samples[i]) * samples[i];
    return sum / static_cast<double>(to - from);
}
}  // namespace

int main(int argc, char** argv) {
    const char* dumpPath = nullptr;
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], "--dump") == 0) dumpPath = argv[i + 1];

    // The engine ships the int8 network: the float weights are upstream's
    // debug copy and made every frame stream 11 MB from memory.
    static RNNoise model;
    require(init_rnnoise(&model, rnnoise_arrays) == 0, "init_rnnoise");
    require(model.gru1_input.float_weights == nullptr && model.gru1_input.weights != nullptr,
            "the int8 network is built, not the debug float weights");

    const int frameSize = rnnoise_get_frame_size();
    require(frameSize == 480, "RNNoise frame size is 480 samples");
    const auto input = testSignal(48000, 20);
    std::vector<float> output(input.size());
    std::vector<float> vad;
    DenoiseState* state = rnnoise_create(nullptr);
    require(state != nullptr, "rnnoise_create");

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t offset = 0; offset + frameSize <= input.size(); offset += frameSize) {
        vad.push_back(rnnoise_process_frame(state, &output[offset], &input[offset]));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    rnnoise_destroy(state);

    for (float sample : output) require(std::isfinite(sample), "finite output");
    for (float probability : vad) require(probability >= 0.0f && probability <= 1.0f, "VAD in range");
    const std::size_t half = input.size() / 2;
    // Skip the first second of each half: the model adapts.
    const double noiseIn = energy(input, 48000, half), noiseOut = energy(output, 48000, half);
    const double voiceIn = energy(input, half + 48000, input.size()), voiceOut = energy(output, half + 48000, input.size());
    require(noiseOut < noiseIn * 0.25, "noise alone is attenuated by more than 6 dB");
    require(voiceOut > voiceIn * 0.05, "a voiced signal is not removed");

    const double perFrameUs = seconds * 1e6 / static_cast<double>(vad.size());
    std::cout << "RNNoise " << vad.size() << " frames, " << perFrameUs << " us per 10 ms frame ("
              << perFrameUs / 100.0 << " % of one core), noise " << 10 * std::log10(noiseOut / noiseIn)
              << " dB, voice " << 10 * std::log10(voiceOut / voiceIn) << " dB.\n";
    if (dumpPath) {
        FILE* file = std::fopen(dumpPath, "wb");
        require(file != nullptr, "open dump");
        std::fwrite(output.data(), sizeof(float), output.size(), file);
        std::fwrite(vad.data(), sizeof(float), vad.size(), file);
        std::fclose(file);
    }
    return 0;
}
