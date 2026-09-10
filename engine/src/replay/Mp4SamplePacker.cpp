#include "clipture/replay/Mp4SamplePacker.hpp"
#include "clipture/H264PacketAnalyzer.hpp"

#include <algorithm>
#include <limits>

namespace clipture::replay {
namespace {

// A per-packet allocation guard, not a replay-window limit. Larger samples keep
// the old path. The spill worker only formats one packet at a time.
constexpr std::size_t maximumPackedSampleBytes = 64u * 1024u * 1024u;

bool writable(uint8_t type) { return type != 7 && type != 8 && type != 9; }

void appendLength(PacketPayload& output, uint32_t length) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((length >> shift) & 0xff));
    }
}

}  // namespace

std::optional<EncodedPacket> packMp4VideoSample(const EncodedPacket& source) {
    if (source.kind != PacketKind::Video || source.codec != PacketCodec::H264AnnexB ||
        !source.payload || source.payload->empty() ||
        source.payload->size() > maximumPackedSampleBytes) return std::nullopt;

    EncodedPacket analyzed = source;
    if (!analyzed.h264.analyzed && !analyzeH264Packet(analyzed)) return std::nullopt;
    const auto& layout = analyzed.h264;
    if (layout.inlineCount > H264PacketLayout::inlineCapacity || layout.nalCount() == 0) {
        return std::nullopt;
    }

    const auto bytes = payloadBytes(source);
    std::size_t packedSize = 0;
    std::size_t previousEnd = 0;
    bool valid = true;
    forEachH264Nal(layout, [&](const H264NalSpan& nalu) {
        if (!valid) return;
        if (nalu.size == 0 || nalu.offset < previousEnd || nalu.offset >= bytes.size() ||
            nalu.size > bytes.size() - nalu.offset ||
            (std::to_integer<uint8_t>(bytes[nalu.offset]) & 0x1f) != nalu.type) {
            valid = false;
            return;
        }
        previousEnd = static_cast<std::size_t>(nalu.offset) + nalu.size;
        if (nalu.type == 7 || nalu.type == 8) {
            valid = nalu.size <= std::numeric_limits<uint16_t>::max();
        } else if (writable(nalu.type)) {
            const uint64_t nextSize = static_cast<uint64_t>(packedSize) + 4 + nalu.size;
            valid = nextSize <= maximumPackedSampleBytes;
            if (valid) packedSize = static_cast<std::size_t>(nextSize);
        }
    });
    if (!valid || packedSize == 0 || packedSize != layout.avccSampleSize) return std::nullopt;

    auto packed = std::make_shared<PacketPayload>();
    packed->reserve(packedSize);
    std::shared_ptr<H264DecoderConfig> config;
    forEachH264Nal(layout, [&](const H264NalSpan& nalu) {
        const auto content = bytes.subspan(nalu.offset, nalu.size);
        if (nalu.type == 7 || nalu.type == 8) {
            if (!config) config = std::make_shared<H264DecoderConfig>();
            auto& parameter = nalu.type == 7 ? config->sps : config->pps;
            if (parameter.empty()) parameter.assign(content.begin(), content.end());
        } else if (writable(nalu.type)) {
            appendLength(*packed, nalu.size);
            packed->insert(packed->end(), content.begin(), content.end());
        }
    });

    analyzed.codec = PacketCodec::H264Avcc;
    analyzed.h264 = {}; // Annex B offsets no longer describe the payload.
    analyzed.h264Config = std::move(config);
    analyzed.payload = std::move(packed);
    analyzed.payloadReader.reset();
    return analyzed;
}

}  // namespace clipture::replay
