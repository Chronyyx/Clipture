#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace clipture {

struct PcmInputLayout {
    int channels = 1;
    int containerBits = 16;
    int validBits = 16;
    std::size_t blockAlign = 2;
    bool floatingPoint = false;
};

namespace detail {

inline int16_t normalizedPcmToS16(double value) {
    if (!std::isfinite(value)) return 0;
    value = std::clamp(value, -1.0, 1.0);
    const double scale = value < 0.0 ? 32768.0 : 32767.0;
    return static_cast<int16_t>(std::clamp<long long>(
        std::llround(value * scale),
        -32768,
        32767));
}

inline uint64_t readLittleEndian(std::span<const std::byte> sample) {
    uint64_t raw = 0;
    for (std::size_t index = 0; index < sample.size(); ++index) {
        raw |= static_cast<uint64_t>(std::to_integer<uint8_t>(sample[index])) << (index * 8);
    }
    return raw;
}

inline int64_t signedIntegerPcmValue(
    std::span<const std::byte> sample,
    int containerBits,
    int validBits) {
    const int paddingBits = containerBits - validBits;
    uint64_t value = readLittleEndian(sample) >> paddingBits;
    if (validBits < 64) {
        const uint64_t signBit = uint64_t { 1 } << (validBits - 1);
        if ((value & signBit) != 0) value |= ~uint64_t { 0 } << validBits;
    }
    return static_cast<int64_t>(value);
}

inline double integerPcmToNormalized(
    std::span<const std::byte> sample,
    int containerBits,
    int validBits) {
    if (containerBits == 8) {
        const int paddingBits = containerBits - validBits;
        const uint64_t value = readLittleEndian(sample) >> paddingBits;
        const int64_t midpoint = int64_t { 1 } << (validBits - 1);
        return std::clamp(
            static_cast<double>(static_cast<int64_t>(value) - midpoint) /
                static_cast<double>(midpoint),
            -1.0,
            1.0);
    }

    const int64_t value = signedIntegerPcmValue(sample, containerBits, validBits);
    return std::clamp(
        static_cast<double>(value) / std::ldexp(1.0, validBits - 1),
        -1.0,
        1.0);
}

inline int16_t integerPcmToS16(
    std::span<const std::byte> sample,
    int containerBits,
    int validBits) {
    if (containerBits == 8) {
        const int paddingBits = containerBits - validBits;
        const int64_t value = static_cast<int64_t>(readLittleEndian(sample) >> paddingBits);
        const int64_t midpoint = int64_t { 1 } << (validBits - 1);
        return static_cast<int16_t>(std::clamp<int64_t>(
            (value - midpoint) * (int64_t { 1 } << (16 - validBits)),
            -32768,
            32767));
    }

    int64_t value = signedIntegerPcmValue(sample, containerBits, validBits);
    if (validBits > 16) {
        value /= int64_t { 1 } << (validBits - 16);
    } else if (validBits < 16) {
        value *= int64_t { 1 } << (16 - validBits);
    }
    return static_cast<int16_t>(std::clamp<int64_t>(value, -32768, 32767));
}

inline bool validatePcmConversion(
    std::size_t inputBytes,
    std::size_t frames,
    const PcmInputLayout& layout,
    int outputChannels,
    std::size_t outputSamples) {
    if (layout.channels <= 0 || outputChannels <= 0 || outputChannels > layout.channels ||
        layout.containerBits <= 0 || layout.containerBits > 64 ||
        layout.containerBits % 8 != 0 || layout.validBits <= 0 ||
        layout.validBits > layout.containerBits) {
        return false;
    }

    const std::size_t containerBytes = static_cast<std::size_t>(layout.containerBits / 8);
    const std::size_t minimumBlockAlign = containerBytes * static_cast<std::size_t>(layout.channels);
    if (layout.blockAlign < minimumBlockAlign ||
        (layout.floatingPoint && layout.containerBits != 32 && layout.containerBits != 64) ||
        frames > std::numeric_limits<std::size_t>::max() / layout.blockAlign ||
        frames > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(outputChannels)) {
        return false;
    }

    return inputBytes >= frames * layout.blockAlign &&
        outputSamples >= frames * static_cast<std::size_t>(outputChannels);
}

inline double sampleToNormalized(
    std::span<const std::byte> sample,
    const PcmInputLayout& layout) {
    if (layout.floatingPoint && layout.containerBits == 32) {
        float value = 0.0f;
        std::memcpy(&value, sample.data(), sizeof(value));
        return std::isfinite(value) ? std::clamp<double>(value, -1.0, 1.0) : 0.0;
    }
    if (layout.floatingPoint) {
        double value = 0.0;
        std::memcpy(&value, sample.data(), sizeof(value));
        return std::isfinite(value) ? std::clamp(value, -1.0, 1.0) : 0.0;
    }
    return integerPcmToNormalized(sample, layout.containerBits, layout.validBits);
}

}  // namespace detail

inline bool convertInterleavedPcmToS16(
    std::span<const std::byte> input,
    std::size_t frames,
    const PcmInputLayout& layout,
    int outputChannels,
    std::span<int16_t> output) {
    if (!detail::validatePcmConversion(
            input.size(), frames, layout, outputChannels, output.size())) return false;

    const std::size_t containerBytes = static_cast<std::size_t>(layout.containerBits / 8);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto frameBytes = input.subspan(frame * layout.blockAlign, layout.blockAlign);
        for (int channel = 0; channel < outputChannels; ++channel) {
            const auto sample = frameBytes.subspan(
                static_cast<std::size_t>(channel) * containerBytes,
                containerBytes);
            int16_t converted = 0;
            if (layout.floatingPoint && layout.containerBits == 32) {
                float value = 0.0f;
                std::memcpy(&value, sample.data(), sizeof(value));
                converted = detail::normalizedPcmToS16(value);
            } else if (layout.floatingPoint) {
                double value = 0.0;
                std::memcpy(&value, sample.data(), sizeof(value));
                converted = detail::normalizedPcmToS16(value);
            } else {
                converted = detail::integerPcmToS16(
                    sample,
                    layout.containerBits,
                    layout.validBits);
            }
            output[frame * static_cast<std::size_t>(outputChannels) + channel] = converted;
        }
    }
    return true;
}

inline bool convertInterleavedPcmToF32(
    std::span<const std::byte> input,
    std::size_t frames,
    const PcmInputLayout& layout,
    int outputChannels,
    std::span<float> output) {
    if (!detail::validatePcmConversion(
            input.size(), frames, layout, outputChannels, output.size())) return false;

    const std::size_t containerBytes = static_cast<std::size_t>(layout.containerBits / 8);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto frameBytes = input.subspan(frame * layout.blockAlign, layout.blockAlign);
        for (int channel = 0; channel < outputChannels; ++channel) {
            const auto sample = frameBytes.subspan(
                static_cast<std::size_t>(channel) * containerBytes,
                containerBytes);
            output[frame * static_cast<std::size_t>(outputChannels) + channel] =
                static_cast<float>(detail::sampleToNormalized(sample, layout));
        }
    }
    return true;
}

}  // namespace clipture
