#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace clipture::mux {

// Uses the existing mux writer's buffering, failure state, pacing and metrics.
// Padding is zero-filled inside mdat but outside every referenced sample.
template<class Writer>
void writePadding(Writer& writer, uint64_t bytes) {
    const std::array<std::byte, 4096> zeros{};
    while (bytes) {
        const auto count = static_cast<std::size_t>(std::min<uint64_t>(bytes, zeros.size()));
        writer.write(std::span(zeros).first(count));
        bytes -= count;
    }
}

} // namespace clipture::mux
