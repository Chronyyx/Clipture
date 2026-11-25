#include "../src/GameFrameProbe.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using clipture::capture::BlankWindowDetector;
using clipture::capture::patchesHaveContent;

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

constexpr UINT kWidth = 160;
constexpr UINT kHeight = 32;

// BGRA rows with padding, as a mapped staging texture has.
std::vector<uint8_t> bgra(uint8_t value) {
    return std::vector<uint8_t>(size_t(kWidth * 4 + 64) * kHeight, value);
}

void bgraPatches() {
    auto black = bgra(0);
    require(!patchesHaveContent(black.data(), kWidth * 4 + 64, kWidth, kHeight, false), "black BGRA has no content");
    auto nearBlack = bgra(8);
    require(!patchesHaveContent(nearBlack.data(), kWidth * 4 + 64, kWidth, kHeight, false), "near-black noise is not content");
    auto onePixel = bgra(0);
    onePixel[size_t(31) * (kWidth * 4 + 64) + 159 * 4 + 2] = 200;  // last pixel, red
    require(patchesHaveContent(onePixel.data(), kWidth * 4 + 64, kWidth, kHeight, false), "one lit pixel is content");
    auto padding = bgra(0);
    padding[kWidth * 4 + 10] = 255;  // in the row padding only
    require(!patchesHaveContent(padding.data(), kWidth * 4 + 64, kWidth, kHeight, false), "row padding is ignored");
}

void halfPatches() {
    const UINT pitch = kWidth * 8;
    std::vector<uint16_t> black(size_t(pitch / 2) * kHeight, 0);
    require(!patchesHaveContent(reinterpret_cast<uint8_t*>(black.data()), pitch, kWidth, kHeight, true), "black scRGB has no content");
    std::vector<uint16_t> negativeZero(black.size(), 0x8000);
    require(!patchesHaveContent(reinterpret_cast<uint8_t*>(negativeZero.data()), pitch, kWidth, kHeight, true), "-0 is black");
    auto grey = black;
    grey[size_t(16) * (pitch / 2) + 80 * 4 + 1] = 0x3800;  // 0.5 green
    require(patchesHaveContent(reinterpret_cast<uint8_t*>(grey.data()), pitch, kWidth, kHeight, true), "mid grey is content");
    auto alphaOnly = black;
    alphaOnly[3] = 0x3C00;  // alpha 1.0 does not count
    require(!patchesHaveContent(reinterpret_cast<uint8_t*>(alphaOnly.data()), pitch, kWidth, kHeight, true), "alpha is ignored");
}

void blankDetector() {
    BlankWindowDetector detector;
    require(!detector.observe(false), "one black probe is not enough (a cut, a fade)");
    require(detector.observe(false), "two black probes in a row mean the window is blank");
    detector.reset();
    require(!detector.observe(false), "reset starts over");
    require(!detector.observe(true), "content clears the count");
    require(!detector.observe(false), "the count restarts after content");
}
}  // namespace

int main() {
    bgraPatches();
    halfPatches();
    blankDetector();
    std::cout << "game frame probe tests passed\n";
    return 0;
}
