#include "clipture/mux/InPlaceMp4Header.hpp"
#include <stdexcept>

namespace clipture::mux {
std::vector<std::byte> inPlaceMp4Header(std::span<const uint8_t> ftyp, uint64_t start, uint64_t end) {
    if (start > 1024 * 1024 || start < ftyp.size() + 24 || end < start || end > INT64_MAX) {
        throw std::invalid_argument("Invalid in-place MP4 header reservation");
    }
    std::vector<std::byte> result(static_cast<std::size_t>(start));
    for (std::size_t i = 0; i < ftyp.size(); ++i) result[i] = static_cast<std::byte>(ftyp[i]);
    const auto put = [&](std::size_t offset, uint64_t value, int bytes) {
        for (int i = bytes - 1; i >= 0; --i) { result[offset + i] = static_cast<std::byte>(value & 255); value >>= 8; }
    };
    const auto type = [&](std::size_t offset, const char* name) {
        for (int i = 0; i < 4; ++i) result[offset + i] = static_cast<std::byte>(name[i]);
    };
    put(ftyp.size(), start - ftyp.size() - 16, 4);
    type(ftyp.size() + 4, "free");
    put(static_cast<std::size_t>(start - 16), 1, 4);
    type(static_cast<std::size_t>(start - 12), "mdat");
    put(static_cast<std::size_t>(start - 8), end - start + 16, 8);
    return result;
}
}
