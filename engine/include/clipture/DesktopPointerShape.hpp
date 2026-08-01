#pragma once

#include <dxgi1_2.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace clipture {

enum class DesktopPointerPixelMode : uint16_t {
    Alpha = 1,
    Replace = 2,
    Xor = 3,
    AndXor = 4,
};

struct DecodedDesktopPointerShape {
    UINT width = 0;
    UINT height = 0;
    std::vector<uint16_t> rgbaOperationPixels;
};

struct DesktopPointerClip {
    UINT destinationX = 0;
    UINT destinationY = 0;
    UINT sourceX = 0;
    UINT sourceY = 0;
    UINT width = 0;
    UINT height = 0;

    explicit operator bool() const { return width > 0 && height > 0; }
};

DesktopPointerClip clipDesktopPointer(
    POINT position,
    UINT pointerWidth,
    UINT pointerHeight,
    UINT outputWidth,
    UINT outputHeight);

bool decodeDesktopPointerShape(
    const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shapeInfo,
    std::span<const std::byte> bytes,
    DecodedDesktopPointerShape& decoded,
    std::string& error);

}  // namespace clipture
