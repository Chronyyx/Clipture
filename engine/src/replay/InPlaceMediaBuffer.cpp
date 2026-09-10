#include "clipture/replay/InPlaceMediaBuffer.hpp"
#include "clipture/replay/Mp4SamplePacker.hpp"
#include "clipture/replay/InPlaceExtent.hpp"
#include "clipture/platform/windows/InPlaceFile.hpp"
#include <stdexcept>

namespace clipture::replay {
namespace {
class Reader final : public PacketPayloadReader {
public:
    explicit Reader(PayloadExtent extent) : extent_(std::move(extent)) {}
    std::size_t size() const noexcept override { return static_cast<std::size_t>(extent_.length); }
    bool read(std::size_t offset, std::span<std::byte> bytes) const override {
        if (offset > extent_.length || bytes.size() > extent_.length - offset) return false;
        return extent_.source->read(extent_.offset + offset, bytes);
    }
    std::optional<PayloadExtent> extent() const override { return extent_; }
private:
    PayloadExtent extent_;
};
}
InPlaceMediaBuffer::InPlaceMediaBuffer(const std::filesystem::path& path)
    : file_(platform::windows::InPlaceFile::create(path)) {
    if (!file_) throw std::runtime_error("Could not create private in-place recording file");
}
bool InPlaceMediaBuffer::append(const EncodedPacket& source) {
    if (frozen_) return false;
    auto packet = source.kind == PacketKind::Audio && source.codec == PacketCodec::AacLc &&
        source.sampleRate > 0 && source.channelCount > 0 && source.audioFrameCount > 0 && source.payload
        ? std::optional<EncodedPacket>(source) : packMp4VideoSample(source);
    if (!packet) return false;
    const auto previous = std::find_if(packets_.rbegin(), packets_.rend(), [&](const auto& item) {
        return item.kind == packet->kind && (item.kind == PacketKind::Video || item.logicalTrackId == packet->logicalTrackId);
    });
    if (previous != packets_.rend() && (packet->pts100ns <= previous->pts100ns ||
        (packet->kind == PacketKind::Video && (packet->encoderEpoch != previous->encoderEpoch ||
        packet->encodedWidth != previous->encodedWidth || packet->encodedHeight != previous->encodedHeight)))) return false;
    const auto length = payloadSize(*packet);
    const auto offset = file_->append(payloadBytes(*packet));
    if (!offset) return false;
    packet->payloadReader = std::make_shared<Reader>(PayloadExtent{file_, *offset, length});
    packet->payload.reset();
    packets_.push_back(std::move(*packet));
    return true;
}
bool InPlaceMediaBuffer::freeze() {
    if (frozen_) return true;
    frozen_ = !packets_.empty() && file_->seal();
    return frozen_;
}
InPlaceMediaBuffer::InPlaceMediaBuffer(std::shared_ptr<platform::windows::InPlaceFile> file)
    : file_(std::move(file)), frozen_(true) {
    if (!file_ || !file_->seal()) throw std::runtime_error("In-place save requires a sealed file");
}
uint64_t InPlaceMediaBuffer::mediaEnd() const { return file_->size(); }
std::optional<uint64_t> InPlaceMediaBuffer::placeSample(const PacketPayloadReaderPtr& reader,
    std::span<const std::byte> memory, std::size_t length) {
    if (!frozen_ || length == 0 || length > 64u * 1024u * 1024u) return std::nullopt;
    if (memory.empty() && reader && reader->size() == length) {
        const auto extent = reader->extent();
        const auto lease = extent ? std::dynamic_pointer_cast<const InPlaceExtent>(extent->source) : nullptr;
        if (extent && (extent->source == file_ || (lease && lease->file() == file_)) &&
            extent->length == length && extent->offset >= platform::windows::InPlaceFile::mediaStart &&
            extent->offset <= file_->size() && length <= file_->size() - extent->offset) return extent->offset;
    }
    // Only missing samples (resident tail, decoder preroll or audio repair) are
    // appended. Already resident file extents are never copied or modified.
    PacketPayload scratch;
    if (memory.empty()) {
        scratch.resize(length);
        if (!reader || !reader->read(0, scratch)) return std::nullopt;
        memory = scratch;
    }
    if (memory.size() != length) return std::nullopt;
    return file_->appendFinalSample(memory);
}
std::optional<InPlaceMediaLayout> InPlaceMediaBuffer::layout(std::span<const EncodedPacket* const> samples) {
    if (!frozen_ || samples.empty()) return std::nullopt;
    InPlaceMediaLayout result{platform::windows::InPlaceFile::mediaStart, file_->size(), {}};
    for (const auto* packet : samples) {
        if (!packet || packet->kind != PacketKind::Video) return std::nullopt;
        auto materialized = *packet;
        if (packet->codec != PacketCodec::H264Avcc && !packet->payload) {
            if (payloadSize(*packet) > 64u * 1024u * 1024u) return std::nullopt;
            materialized.payload = std::make_shared<PacketPayload>(payloadSize(*packet));
            if (!readPayload(*packet, 0, *materialized.payload)) return std::nullopt;
        }
        auto prepared = packet->codec == PacketCodec::H264Avcc ? std::optional<EncodedPacket>(*packet) : packMp4VideoSample(materialized);
        if (!prepared) return std::nullopt;
        const auto offset = placeSample(prepared->payloadReader, payloadBytes(*prepared), payloadSize(*prepared));
        if (!offset) return std::nullopt;
        result.sampleOffsets.push_back(*offset);
    }
    result.mediaEnd = file_->size();
    return result;
}
bool InPlaceMediaBuffer::finalize(std::span<const std::byte> prefix, std::span<const std::byte> index) {
    return frozen_ && file_->finalize(prefix, index);
}
bool InPlaceMediaBuffer::publish(const std::filesystem::path& destination) { return file_->publish(destination); }
InPlaceIo InPlaceMediaBuffer::io() const { return file_->io(); }
} // namespace clipture::replay
