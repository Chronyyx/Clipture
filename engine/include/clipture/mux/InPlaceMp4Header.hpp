#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace clipture::mux {
// ftyp + free padding + 64-bit mdat header, ending exactly at mediaStart.
std::vector<std::byte> inPlaceMp4Header(std::span<const uint8_t> ftyp, uint64_t mediaStart, uint64_t mediaEnd);
}
