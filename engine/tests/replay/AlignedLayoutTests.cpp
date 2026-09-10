#include "ExtentTestSupport.hpp"
#include "clipture/mux/PreparedVideoLayout.hpp"

namespace replay_tests {
using namespace clipture;
using namespace clipture::replay;

void testAlignedLayout() {
    const auto source = std::make_shared<PatternSource>(UINT64_MAX);
    for (uint64_t start = 0; start < 16; ++start) {
        for (uint64_t destination = 0; destination < 16; ++destination) {
            for (uint64_t length = 2; length <= 40; ++length) {
                const std::vector<PayloadExtent> samples{{source, start, 1}, {source, start + 1, length - 1}};
                const auto plan = planPayloadLayout(samples, destination, 8, PayloadPlacement::AlignRegions);
                require(plan.payloadBytes() == length && plan.paddingBytes() < 8,
                        "at most one bounded gap per contiguous region, never per sample");
                require(plan.sampleOffsets()[1] == plan.sampleOffsets()[0] + 1,
                        "adjacent samples remain adjacent");
                if (plan.paddingBytes()) require(plan.alignmentCandidateBytes() > plan.paddingBytes(),
                                                 "do not pad without a larger eligible body");
                VectorSink sink(static_cast<std::size_t>(plan.outputEnd() + 1));
                std::array<std::byte, 3> scratch;
                const auto copied = copyPayload(plan, sink, scratch);
                require(copied.ok() && copied.completedBytes == length && copied.paddingBytes == plan.paddingBytes(),
                        "media and zero padding accounted separately");
                for (uint64_t i = destination; i < plan.sampleOffsets()[0]; ++i) {
                    require(sink.output[static_cast<std::size_t>(i)] == std::byte{0}, "every gap explicitly zeroed");
                }
                for (uint64_t i = 0; i < length; ++i) {
                    require(sink.output[static_cast<std::size_t>(plan.sampleOffsets()[0] + i)] ==
                            static_cast<std::byte>((start + i) % 251), "sample bytes unchanged by aligned placement");
                }
                require(sink.output.back() == std::byte{0xff}, "no tail overwrite");
                for (const auto& transfer : plan.transfers()) {
                    if (transfer.kind == PayloadTransferKind::AlignmentCandidate) {
                        require(transfer.input.offset % 8 == 0 && transfer.outputOffset % 8 == 0 &&
                                transfer.input.length % 8 == 0, "candidate addresses and lengths align");
                    }
                }
            }
        }
    }
    const auto other = std::make_shared<PatternSource>(UINT64_MAX);
    const std::vector<PayloadExtent> separate{{source, 0, 9000}, {other, 0, 12000}};
    const auto plan = planPayloadLayout(separate, 48, 4096, PayloadPlacement::AlignRegions);
    require(plan.sampleOffsets() == std::vector<uint64_t>{4096, 16384} && plan.paddingBytes() == 7336 &&
            plan.alignmentCandidateBytes() == 16384, "separate source regions align independently");
    VectorSink broken(static_cast<std::size_t>(plan.outputEnd()));
    broken.failWrite = 0;
    std::array<std::byte, 7> scratch;
    const auto failed = copyPayload(plan, broken, scratch);
    require(failed.error == PayloadCopyError::WriteFailed && failed.completedBytes == 0 && failed.paddingBytes == 0,
            "failure writing gap stops before reading or writing sample bytes");
    const std::vector<PayloadExtent> tiny{{source, 0, 3}};
    require(planPayloadLayout(tiny, 1, 4096, PayloadPlacement::AlignRegions).paddingBytes() == 0,
            "small regions stay compact instead of wasting a block");
    requireThrows<std::invalid_argument>([&] { planPayloadLayout(tiny, 0, 0, PayloadPlacement::AlignRegions); });
    const std::vector<PayloadExtent> boundary{{source, 0, 4096}};
    requireThrows<std::overflow_error>([&] {
        planPayloadLayout(boundary, UINT64_MAX - 4096, 4096, PayloadPlacement::AlignRegions);
    });
    const std::vector<PayloadExtent> large{{source, 65536, 1ULL << 33}};
    const auto largePlan = planPayloadLayout(large, 48, 65536, PayloadPlacement::AlignRegions);
    require(largePlan.outputEnd() == (1ULL << 33) + 65536 && largePlan.sampleOffsets()[0] == 65536,
            "aligned offsets remain 64 bit without allocating payload");

    class Reader final : public PacketPayloadReader {
    public:
        explicit Reader(PayloadExtent extent) : extent_(std::move(extent)) {}
        std::size_t size() const noexcept override { return static_cast<std::size_t>(extent_.length); }
        bool read(std::size_t offset, std::span<std::byte> bytes) const override {
            return extent_.source->read(extent_.offset + offset, bytes);
        }
        std::optional<PayloadExtent> extent() const override { return extent_; }
    private:
        PayloadExtent extent_;
    };
    EncodedPacket packet;
    packet.codec = PacketCodec::H264Avcc;
    packet.payloadReader = std::make_shared<Reader>(boundary.front());
    const std::vector<const EncodedPacket*> selection{&packet};
    require(mux::preparedVideoLayout(selection, 48, 4096).has_value(), "eligible disk sample plans");
    require(!mux::preparedVideoLayout(selection, 48, 3), "invalid alignment falls back");
    packet.codec = PacketCodec::H264AnnexB;
    require(!mux::preparedVideoLayout(selection, 48, 4096), "legacy sample falls back");
    packet.codec = PacketCodec::H264Avcc;
    packet.payload = std::make_shared<PacketPayload>(4096);
    require(!mux::preparedVideoLayout(selection, 48, 4096), "resident sample falls back");
}

} // namespace replay_tests
