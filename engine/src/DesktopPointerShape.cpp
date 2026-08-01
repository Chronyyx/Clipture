#include "clipture/DesktopPointerShape.hpp"

#include <algorithm>
#include <limits>

namespace clipture {
namespace {

constexpr uint16_t control(DesktopPointerPixelMode mode, uint8_t alpha = 0) {
    return static_cast<uint16_t>((static_cast<uint16_t>(mode) << 8) | alpha);
}

bool checkedStorageSize(UINT width, UINT height, std::size_t& value) {
    if (width == 0 || height == 0) return false;
    constexpr std::size_t channels = 4;
    if (static_cast<std::size_t>(width) > std::numeric_limits<std::size_t>::max() / height) return false;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (pixels > std::numeric_limits<std::size_t>::max() / channels) return false;
    value = pixels * channels;
    return true;
}

void setPixel(
    DecodedDesktopPointerShape& decoded,
    UINT x,
    UINT y,
    uint16_t red,
    uint16_t green,
    uint16_t blue,
    uint16_t operation) {
    const std::size_t offset = (static_cast<std::size_t>(y) * decoded.width + x) * 4;
    decoded.rgbaOperationPixels[offset + 0] = red;
    decoded.rgbaOperationPixels[offset + 1] = green;
    decoded.rgbaOperationPixels[offset + 2] = blue;
    decoded.rgbaOperationPixels[offset + 3] = operation;
}

}  // namespace

DesktopPointerClip clipDesktopPointer(
    POINT position,
    UINT pointerWidth,
    UINT pointerHeight,
    UINT outputWidth,
    UINT outputHeight) {
    const int64_t left = std::max<int64_t>(0, position.x);
    const int64_t top = std::max<int64_t>(0, position.y);
    const int64_t right = std::min<int64_t>(
        outputWidth,
        static_cast<int64_t>(position.x) + pointerWidth);
    const int64_t bottom = std::min<int64_t>(
        outputHeight,
        static_cast<int64_t>(position.y) + pointerHeight);
    if (right <= left || bottom <= top) return {};
    return {
        static_cast<UINT>(left),
        static_cast<UINT>(top),
        static_cast<UINT>(left - position.x),
        static_cast<UINT>(top - position.y),
        static_cast<UINT>(right - left),
        static_cast<UINT>(bottom - top),
    };
}

bool decodeDesktopPointerShape(
    const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shapeInfo,
    std::span<const std::byte> bytes,
    DecodedDesktopPointerShape& decoded,
    std::string& error) {
    decoded = {};
    error.clear();

    UINT visibleHeight = shapeInfo.Height;
    if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        if (shapeInfo.Height == 0 || (shapeInfo.Height % 2) != 0) {
            error = "Monochrome pointer height must contain equal AND and XOR masks.";
            return false;
        }
        visibleHeight /= 2;
    }

    std::size_t storageSize = 0;
    if (!checkedStorageSize(shapeInfo.Width, visibleHeight, storageSize)) {
        error = "Pointer dimensions are invalid or too large.";
        return false;
    }
    if (shapeInfo.Pitch == 0 || static_cast<std::size_t>(shapeInfo.Pitch) * shapeInfo.Height > bytes.size()) {
        error = "Pointer shape buffer is smaller than its declared pitch and height.";
        return false;
    }

    decoded.width = shapeInfo.Width;
    decoded.height = visibleHeight;
    decoded.rgbaOperationPixels.assign(storageSize, 0);

    if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ||
        shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        if (shapeInfo.Pitch < shapeInfo.Width * 4) {
            error = "Color pointer pitch is too small.";
            decoded = {};
            return false;
        }
        for (UINT y = 0; y < visibleHeight; ++y) {
            const auto* row = reinterpret_cast<const uint8_t*>(bytes.data()) + static_cast<std::size_t>(y) * shapeInfo.Pitch;
            for (UINT x = 0; x < shapeInfo.Width; ++x) {
                const uint8_t blue = row[x * 4 + 0];
                const uint8_t green = row[x * 4 + 1];
                const uint8_t red = row[x * 4 + 2];
                const uint8_t alpha = row[x * 4 + 3];
                if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                    setPixel(decoded, x, y, red, green, blue, control(DesktopPointerPixelMode::Alpha, alpha));
                } else if (alpha == 0) {
                    setPixel(decoded, x, y, red, green, blue, control(DesktopPointerPixelMode::Replace));
                } else if (alpha == 0xFF) {
                    setPixel(decoded, x, y, red, green, blue, control(DesktopPointerPixelMode::Xor));
                } else {
                    error = "Masked-color pointer contains an unsupported alpha mask.";
                    decoded = {};
                    return false;
                }
            }
        }
        return true;
    }

    if (shapeInfo.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        error = "Pointer shape type is unsupported.";
        decoded = {};
        return false;
    }

    const UINT minimumPitch = (shapeInfo.Width + 7) / 8;
    if (shapeInfo.Pitch < minimumPitch) {
        error = "Monochrome pointer pitch is too small.";
        decoded = {};
        return false;
    }
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    const std::size_t xorOffset = static_cast<std::size_t>(shapeInfo.Pitch) * visibleHeight;
    for (UINT y = 0; y < visibleHeight; ++y) {
        const auto* andRow = data + static_cast<std::size_t>(y) * shapeInfo.Pitch;
        const auto* xorRow = data + xorOffset + static_cast<std::size_t>(y) * shapeInfo.Pitch;
        for (UINT x = 0; x < shapeInfo.Width; ++x) {
            const uint8_t bit = static_cast<uint8_t>(0x80u >> (x & 7u));
            const uint16_t andBit = (andRow[x / 8] & bit) != 0 ? 1 : 0;
            const uint16_t xorBit = (xorRow[x / 8] & bit) != 0 ? 1 : 0;
            setPixel(decoded, x, y, andBit, xorBit, 0, control(DesktopPointerPixelMode::AndXor));
        }
    }
    return true;
}

}  // namespace clipture
