#pragma once

#include <cstdint>

// Pixel layout of the frame counter stamped into every presented frame.
// scripts/performance/stress/frame-stamps.cjs decodes the same layout; change
// both together.
namespace clipture::stress {

inline constexpr int kStampOriginX = 24;
inline constexpr int kStampOriginY = 24;
inline constexpr int kStampCellWidth = 24;
inline constexpr int kStampCellHeight = 32;
inline constexpr int kStampMargin = 8;
// Cell 0 is white and cell 1 is black (threshold references), followed by
// 32 payload bits, most significant first.
inline constexpr int kStampReferenceCells = 2;
inline constexpr int kStampPayloadBits = 32;
inline constexpr int kStampCells = kStampReferenceCells + kStampPayloadBits;

// Low 24 bits of the counter plus an 8-bit multiplicative check, so a misread
// bit is rejected instead of being reported as a different frame.
inline uint32_t stampCheck(uint32_t counter) {
    return ((counter & 0xFFFFFFu) * 0x9E3779B1u) >> 24;
}

inline uint32_t stampPayload(uint32_t counter) {
    return ((counter & 0xFFFFFFu) << 8) | stampCheck(counter);
}

}  // namespace clipture::stress
