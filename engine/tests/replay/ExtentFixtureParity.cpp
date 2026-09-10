#include "TestSupport.hpp"
#include "clipture/replay/CopyPayloadWriter.hpp"

#include <iostream>

namespace replay_tests {
namespace {

uint32_t readU32(const std::vector<char>& bytes, std::size_t offset) {
    require(offset <= bytes.size() && bytes.size() - offset >= 4, "bounded fixture box read");
    uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    return value;
}

class FixtureSink final : public clipture::replay::PayloadCopySink {
public:
    FixtureSink(std::ostream& output, uint64_t start) : output_(output), next_(start) {}
    bool write(uint64_t offset, std::span<const std::byte> bytes) override {
        if (offset != next_) return false;
        output_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output_) return false;
        next_ += bytes.size();
        return true;
    }
private:
    std::ostream& output_;
    uint64_t next_;
};

} // namespace

void verifyExtentFixture(const std::vector<clipture::EncodedPacket>& snapshot,
                         const std::filesystem::path& reference, const std::filesystem::path& outputPath) {
    using namespace clipture::replay;
    // Use the existing muxer's metadata as an oracle; this is NOT a new MP4
    // metadata builder. Only replace its entire mdat payload through the planner.
    const auto baseline = readFile(reference);
    std::size_t payloadStart = 0, payloadEnd = 0;
    for (std::size_t box = 0; box < baseline.size();) {
        require(baseline.size() - box >= 8, "complete fixture box header");
        const auto length = readU32(baseline, box);
        require(length >= 8 && length <= baseline.size() - box, "small bounded fixture box");
        if (std::string_view(baseline.data() + box + 4, 4) == "mdat") {
            require(payloadStart == 0, "one fixture mdat");
            payloadStart = box + 8;
            payloadEnd = box + length;
        }
        box += length;
    }
    require(payloadStart != 0, "fixture mdat exists");
    std::vector<PayloadExtent> samples;
    for (const auto& packet : snapshot) {
        require(packet.codec == clipture::PacketCodec::H264Avcc && packet.payloadReader,
                "extent fixture uses prepared disk video only");
        const auto extent = packet.payloadReader->extent();
        require(extent.has_value(), "persisted fixture exposes pinned extent");
        samples.push_back(*extent);
    }
    const auto plan = planPayloadLayout(samples, payloadStart, 4096); // Illustrative geometry, NOT queried volume capability.
    require(plan.outputEnd() == payloadEnd && plan.sampleOffsets().size() == snapshot.size(),
            "planned payload size and sample count match existing mux metadata");
    require(!std::filesystem::exists(outputPath), "never replace a prior fixture output");
    std::ofstream output(outputPath, std::ios::binary);
    output.write(baseline.data(), static_cast<std::streamsize>(payloadStart));
    require(output.good(), "write reference metadata prefix");
    FixtureSink sink(output, payloadStart);
    std::array<std::byte, 64 * 1024> scratch;
    const auto copied = copyPayload(plan, sink, scratch);
    require(copied.ok() && copied.completedBytes == plan.payloadBytes(), "bounded extent copy writes complete fixture payload");
    output.write(baseline.data() + payloadEnd, static_cast<std::streamsize>(baseline.size() - payloadEnd));
    output.close();
    require(!output.fail() && readFile(outputPath) == baseline, "planned MP4 must be byte-identical to existing mux output");
    std::cout << "Extent fixture: " << samples.size() << " samples, " << plan.transfers().size()
              << " transfers, " << plan.alignmentCandidateBytes() << " geometric candidate bytes; all copied.\n";
}

} // namespace replay_tests
