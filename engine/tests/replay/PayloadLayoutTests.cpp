#include "ExtentTestSupport.hpp"

namespace replay_tests {
using namespace clipture::replay;

void testPayloadLayout() {
    const auto source = std::make_shared<PatternSource>(1024);
    const std::vector<PayloadExtent> samples{{source, 3, 10}, {source, 13, 20}};
    const auto plan = planPayloadLayout(samples, 3, 8);
    require(plan.sampleOffsets() == std::vector<uint64_t>{3, 13}, "coalescing preserves sample offsets");
    require(plan.transfers().size() == 3 && plan.alignmentCandidateBytes() == 24,
            "coalesce before splitting copied head, aligned body and copied tail");
    require(plan.transfers()[0].input.length == 5 && plan.transfers()[2].input.length == 1,
            "unaligned boundaries do not add padding or expose neighboring bytes");
    require(plan.outputEnd() == 33 && plan.payloadBytes() == 30, "compact payload layout");
    const auto portable = planPayloadLayout(samples, 3);
    require(portable.transfers().size() == 1 && portable.alignmentCandidateBytes() == 0,
            "zero alignment disables candidates, not coalescing");
    const auto mismatch = planPayloadLayout(samples, 4, 8);
    require(mismatch.transfers().size() == 1 && mismatch.alignmentCandidateBytes() == 0,
            "different source/destination residues require copying");
    const auto other = std::make_shared<PatternSource>(1024);
    const std::vector<PayloadExtent> disjoint{{source, 0, 8}, {other, 8, 8}, {source, 16, 8}, {source, 4, 8}};
    require(planPayloadLayout(disjoint, 0, 8).transfers().size() == 4,
            "do not merge distinct sources, gaps or overlapping source ranges");

    // Exhaustively compare small layouts to a direct byte oracle. This covers
    // every alignment residue, short bodies and sample boundaries inside blocks.
    for (uint64_t start = 0; start < 16; ++start) {
        for (uint64_t destination = 0; destination < 16; ++destination) {
            for (uint64_t length = 2; length <= 40; ++length) {
                const std::vector<PayloadExtent> pair{{source, start, 1}, {source, start + 1, length - 1}};
                const auto generated = planPayloadLayout(pair, destination, 8);
                uint64_t inputCursor = start, outputCursor = destination, candidates = 0;
                for (const auto& transfer : generated.transfers()) {
                    require(transfer.input.offset == inputCursor && transfer.outputOffset == outputCursor,
                            "every requested byte appears exactly once, in order");
                    require(transfer.input.length > 0, "no empty transfer");
                    if (transfer.kind == PayloadTransferKind::AlignmentCandidate) {
                        require(inputCursor % 8 == 0 && outputCursor % 8 == 0 && transfer.input.length % 8 == 0,
                                "candidate source, destination and length are aligned");
                        candidates += transfer.input.length;
                    }
                    inputCursor += transfer.input.length;
                    outputCursor += transfer.input.length;
                }
                require(inputCursor == start + length && outputCursor == destination + length &&
                        candidates == generated.alignmentCandidateBytes(), "complete coverage and candidate accounting");
                VectorSink sink(static_cast<std::size_t>(outputCursor + 1));
                std::array<std::byte, 7> scratch;
                require(copyPayload(generated, sink, scratch).ok(), "portable copy of generated layout");
                for (uint64_t i = 0; i < length; ++i) {
                    require(sink.output[static_cast<std::size_t>(destination + i)] == static_cast<std::byte>((start + i) % 251),
                            "generated copy matches source bytes");
                }
                require(sink.output.back() == std::byte{0xff}, "copy cannot overrun payload end");
                if (destination) require(sink.output[static_cast<std::size_t>(destination - 1)] == std::byte{0xff},
                                         "copy cannot overwrite reserved metadata");
            }
        }
    }

    requireThrows<std::invalid_argument>([&] { planPayloadLayout(samples, 0, 3); });
    const std::vector<PayloadExtent> null{{nullptr, 0, 1}}, empty{{source, 0, 0}}, past{{source, 1024, 1}};
    requireThrows<std::invalid_argument>([&] { planPayloadLayout(null, 0); });
    requireThrows<std::invalid_argument>([&] { planPayloadLayout(empty, 0); });
    requireThrows<std::out_of_range>([&] { planPayloadLayout(past, 0); });
    requireThrows<std::overflow_error>([&] { planPayloadLayout(samples, UINT64_MAX - 10); });
    const auto huge = std::make_shared<PatternSource>(UINT64_MAX);
    const std::vector<PayloadExtent> overflow{{huge, UINT64_MAX - 2, 4}};
    requireThrows<std::out_of_range>([&] { planPayloadLayout(overflow, 0); });
    const std::vector<PayloadExtent> large{{huge, (1ULL << 33), (1ULL << 33)}};
    const auto largePlan = planPayloadLayout(large, 1ULL << 34, 65536);
    require(largePlan.outputEnd() == (1ULL << 34) + (1ULL << 33) && largePlan.transfers().size() == 1,
            "64-bit offsets without per-byte allocation; backend must later split API-size limits");
    require(planPayloadLayout({}, 99, 8).outputEnd() == 99, "empty layout preserves output start");
}

} // namespace replay_tests
