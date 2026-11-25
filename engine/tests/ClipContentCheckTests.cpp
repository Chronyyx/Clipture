#include "clipture/ClipContentCheck.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using clipture::checkClipContent;
using clipture::EncodedPacket;

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

// A clip of `seconds` at 120 fps, keyframe every 2 s; sizes as NVENC wrote
// them for 1440p at 40 Mbps (measured with ffmpeg h264_nvenc).
std::vector<EncodedPacket> clip(int seconds, size_t keyBytes, size_t frameBytes, bool newSourceFrames) {
    std::vector<EncodedPacket> packets;
    const int frames = seconds * 120;
    for (int i = 0; i < frames; ++i) {
        EncodedPacket packet;
        packet.pts100ns = int64_t(i) * 83'333;
        packet.duration100ns = 83'333;
        packet.keyframe = i % 240 == 0;
        packet.encodedWidth = 2560;
        packet.encodedHeight = 1440;
        packet.sourceFrameSequence = newSourceFrames ? uint64_t(i + 1) : 7;
        packet.payload = std::make_shared<clipture::PacketPayload>(packet.keyframe ? keyBytes : frameBytes);
        packets.push_back(std::move(packet));
    }
    return packets;
}

void blackGameplay() {
    // The Deadlock case: the game presents new (black) frames all along.
    const auto packets = clip(90, 1'033, 473, true);
    const auto result = checkClipContent(packets);
    require(result.blank, "1 KB keyframes at 1440p are a black clip");
    require(!result.frozen, "new source frames kept arriving: black, not frozen");
    require(result.maxKeyframeBytes == 1'033, "largest keyframe is reported");
}

void darkButRealGameplay() {
    const auto result = checkClipContent(clip(90, 153'610, 50'578, true));
    require(!result.blank && !result.frozen, "a dark, busy scene is a normal clip");
}

void oneFrameForTwoMinutes() {
    const auto result = checkClipContent(clip(120, 204'838, 1'485, false));
    require(result.frozen, "one source frame for two minutes is frozen");
    require(!result.blank, "a frozen picture with content is not black");
    require(result.distinctSourceFrames == 1, "distinct source frames are counted");
}

void shortOrUnnumberedClips() {
    require(!checkClipContent(clip(5, 204'838, 1'485, false)).frozen, "under 10 s a still picture is not judged frozen");
    auto unnumbered = clip(30, 204'838, 1'485, false);
    for (auto& packet : unnumbered) packet.sourceFrameSequence = 0;
    require(!checkClipContent(unnumbered).frozen, "frames without source numbers are never judged frozen");
    require(!checkClipContent({}).analyzed, "an empty clip is not analyzed");
}

void scalesWithResolution() {
    auto packets = clip(10, 7'000, 400, true);
    for (auto& packet : packets) { packet.encodedWidth = 1280; packet.encodedHeight = 720; }
    require(!checkClipContent(packets).blank, "7 KB keyframes are a real 720p picture");
    for (auto& packet : packets) { packet.encodedWidth = 3840; packet.encodedHeight = 2160; }
    require(checkClipContent(packets).blank, "7 KB keyframes at 4K are a black picture");
}
}  // namespace

int main() {
    blackGameplay();
    darkButRealGameplay();
    oneFrameForTwoMinutes();
    shortOrUnnumberedClips();
    scalesWithResolution();
    std::cout << "clip content check tests passed\n";
    return 0;
}
