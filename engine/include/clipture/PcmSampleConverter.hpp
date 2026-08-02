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
    return static_cast<int16_t>(std::clamp<long long>(
        std::llround(value * 32767.0),
        -32768,
        32767));
}

inline int16_t integerPcmToS16(std::span<const std::byte> sample, int containerBits) {
    if (containerBits == 8) {
        return static_cast<int16_t>(
            (static_cast<int>(std::to_integer<uint8_t>(sample.front())) - 128) << 8);
    }

    uint64_t raw = 0;
    for (std::size_t index = 0; index < sample.size(); ++index) {
        raw |= static_cast<uint64_t>(std::to_integer<uint8_t>(sample[index])) << (index * 8);
    }

    int64_t value = 0;
    if (containerBits == 64) {
        value = static_cast<int64_t>(raw);
    } else {
        const uint64_t signBit = uint64_t { 1 } << (containerBits - 1);
        if ((raw & signBit) != 0) raw |= ~uint64_t { 0 } << containerBits;
        value = static_cast<int64_t>(raw);
    }

    if (containerBits > 16) {
        value >>= containerBits - 16;
    } else if (containerBits < 16) {
        value <<= 16 - containerBits;
    }
    return static_cast<int16_t>(std::clamp<int64_t>(value, -32768, 32767));
}

}  // namespace detail

inline bool convertInterleavedPcmToS16(
    std::span<const std::byte> input,
    std::size_t frames,
    const PcmInputLayout& layout,
    int outputChannels,
    std::span<int16_t> output) {
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

    const std::size_t requiredInputBytes = frames * layout.blockAlign;
    const std::size_t requiredOutputSamples = frames * static_cast<std::size_t>(outputChannels);
    if (input.size() < requiredInputBytes || output.size() < requiredOutputSamples) return false;

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
                converted = detail::integerPcmToS16(sample, layout.containerBits);
            }
            output[frame * static_cast<std::size_t>(outputChannels) + channel] = converted;
        }
    }
    return true;
}

}  // namespace clipture
