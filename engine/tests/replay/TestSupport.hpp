#pragma once

#include "clipture/H264PacketAnalyzer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace replay_tests {

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline clipture::PacketPayload bytes(std::initializer_list<uint8_t> values) {
    clipture::PacketPayload result;
    for (auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

inline clipture::EncodedPacket fixturePacket() {
    clipture::EncodedPacket packet;
    packet.payload = std::make_shared<clipture::PacketPayload>(bytes({
        0, 0, 0, 1, 0x09, 0xf0,
        0, 0, 1, 0x67, 0x64, 0, 0x28,
        0, 0, 0, 1, 0x68, 0xee, 0x3c, 0x80,
        0, 0, 1, 0x06, 0x05, 0xaa,
        0, 0, 0, 1, 0x65, 0xab, 0xcd,
        0, 0, 1, 0x61, 0xef
    }));
    packet.pts100ns = 10'000'000;
    packet.dts100ns = packet.pts100ns;
    packet.duration100ns = 333'333;
    packet.encodedWidth = 160;
    packet.encodedHeight = 90;
    packet.encoderEpoch = 7;
    packet.sourceFrameSequence = 123;
    packet.sourceId = "fixture";
    require(clipture::analyzeH264Packet(packet), "fixture must analyze");
    return packet;
}

inline std::vector<char> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "open test file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct ScratchDirectory {
    std::filesystem::path path;
    explicit ScratchDirectory(const std::filesystem::path& parent = {}) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto root = parent.empty() ? std::filesystem::temp_directory_path() : std::filesystem::canonical(parent);
        require(std::filesystem::is_directory(root), "scratch parent must already exist");
        path = root / ("clipture-mp4-ready-" + std::to_string(tick));
        require(std::filesystem::create_directory(path), "create exclusive scratch directory");
    }
    ~ScratchDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored); // Only the directory created above.
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
};

void testPacker();
void testStoreAndMux();
void testPayloadLayout();
void testPayloadCopy();
void testExtentStore();
void testAlignedLayout();
void testClonePolicy();
void testCloneJob();
void testInPlace();
void testInPlaceArchive();
void verifyInPlaceRollingFixture(const std::vector<clipture::EncodedPacket>& source, const std::filesystem::path& root);
void runInPlaceAudioFixture(const std::vector<clipture::EncodedPacket>& video, const std::filesystem::path& root);
void verifyInPlaceFixture(const std::vector<clipture::EncodedPacket>& packets, const std::filesystem::path& root);
void testWindowsClone(const std::filesystem::path& parent = {});
void verifyWindowsCloneFixture(const std::filesystem::path& source, const std::filesystem::path& output);
void verifyNativeCloneRetry(const std::filesystem::path& source, const std::filesystem::path& output);
void verifyExtentFixture(const std::vector<clipture::EncodedPacket>& snapshot,
                         const std::filesystem::path& reference, const std::filesystem::path& output);
void runFixtureParity(const std::filesystem::path& input, const std::filesystem::path& output);

}  // namespace replay_tests
