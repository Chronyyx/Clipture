#pragma once

// What a saved clip shows, judged at save from the encoded video alone: no
// decode, no capture-time probes. The save already walks every packet.
//
// Black or flat picture: H.264 keyframes of a black 1440p frame are ~1 KB,
// any real picture (even a dark game) 100 KB or more, so a clip whose every
// keyframe stays under 1/256 byte per pixel shows nothing.
//
// Frozen: the capture hands the encoder a source-frame sequence number; a
// clip of 10 s or more built from at most two distinct source frames is one
// picture repeated.

#include "clipture/PacketRingBuffer.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace clipture {

struct ClipContentCheck {
    bool analyzed = false;
    double spanSeconds = 0.0;
    uint64_t frames = 0;
    uint64_t keyframes = 0;
    // 0 when the capture did not number its frames.
    uint64_t distinctSourceFrames = 0;
    uint64_t maxKeyframeBytes = 0;
    uint64_t meanFrameBytes = 0;
    int width = 0;
    int height = 0;
    bool blank = false;
    bool frozen = false;
};

ClipContentCheck checkClipContent(std::span<const EncodedPacket> videoPackets);

// Where the picture came from when the clip was saved. Filled by the engine.
struct ClipSourceFacts {
    bool recordingGameWindow = false;
    double sourceSeconds = 0.0;  // how long that source had been recorded
    bool recordedMonitorPrimary = true;
    bool foregroundOnRecordedMonitor = true;
};

std::string toJson(const ClipContentCheck& content, const ClipSourceFacts& source);

}  // namespace clipture
