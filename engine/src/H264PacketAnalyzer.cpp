#include "clipture/H264PacketAnalyzer.hpp"

#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace clipture {
namespace {

bool startCodeAt(std::span<const std::byte> data, std::size_t offset, std::size_t& size) {
    if (offset + 3 <= data.size() && data[offset] == std::byte { 0 } &&
        data[offset + 1] == std::byte { 0 } && data[offset + 2] == std::byte { 1 }) {
        size = 3;
        return true;
    }
    if (offset + 4 <= data.size() && data[offset] == std::byte { 0 } &&
        data[offset + 1] == std::byte { 0 } && data[offset + 2] == std::byte { 0 } &&
        data[offset + 3] == std::byte { 1 }) {
        size = 4;
        return true;
    }
    return false;
}

std::size_t findStartCode(std::span<const std::byte> data, std::size_t offset, std::size_t& size) {
    for (std::size_t i = offset; i + 3 <= data.size(); ++i) {
        if (startCodeAt(data, i, size)) return i;
    }
    return std::string::npos;
}

}  // namespace

bool analyzeH264Packet(EncodedPacket& packet) {
    packet.codec = PacketCodec::H264AnnexB;
    packet.h264 = {};
    packet.h264.analyzed = true;

    const auto data = payloadBytes(packet);
    if (data.empty() || data.size() > std::numeric_limits<uint32_t>::max()) return false;

    std::vector<H264NalSpan> overflow;
    std::size_t offset = 0;
    uint64_t avccSize = 0;
    while (offset < data.size()) {
        std::size_t startCodeSize = 0;
        const std::size_t start = findStartCode(data, offset, startCodeSize);
        if (start == std::string::npos) break;
        const std::size_t nalStart = start + startCodeSize;
        if (nalStart >= data.size()) break;

        std::size_t nextStartCodeSize = 0;
        const std::size_t next = findStartCode(data, nalStart, nextStartCodeSize);
        const std::size_t nalEnd = next == std::string::npos ? data.size() : next;
        if (nalEnd > nalStart && nalEnd - nalStart <= std::numeric_limits<uint32_t>::max()) {
            H264NalSpan nalu {
                static_cast<uint32_t>(nalStart),
                static_cast<uint32_t>(nalEnd - nalStart),
                static_cast<uint8_t>(std::to_integer<uint8_t>(data[nalStart]) & 0x1F)
            };
            if (packet.h264.inlineCount < H264PacketLayout::inlineCapacity) {
                packet.h264.inlineNalus[packet.h264.inlineCount++] = nalu;
            } else {
                overflow.push_back(nalu);
            }

            packet.h264.hasIdr = packet.h264.hasIdr || nalu.type == 5;
            packet.h264.hasSps = packet.h264.hasSps || nalu.type == 7;
            packet.h264.hasPps = packet.h264.hasPps || nalu.type == 8;
            packet.h264.hasAud = packet.h264.hasAud || nalu.type == 9;
            if (nalu.type != 7 && nalu.type != 8 && nalu.type != 9) {
                avccSize += sizeof(uint32_t) + nalu.size;
            }
        }
        offset = nalEnd;
    }

    if (!overflow.empty()) {
        packet.h264.overflowNalus = std::make_shared<const std::vector<H264NalSpan>>(std::move(overflow));
    }
    if (avccSize <= std::numeric_limits<uint32_t>::max()) {
        packet.h264.avccSampleSize = static_cast<uint32_t>(avccSize);
    }
    packet.keyframe = packet.keyframe || packet.h264.hasIdr;
    return packet.h264.nalCount() > 0;
}

}  // namespace clipture
