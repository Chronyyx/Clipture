#include "ExtentTestSupport.hpp"

namespace replay_tests {
using namespace clipture::replay;

void testPayloadCopy() {
    auto source = std::make_shared<PatternSource>(1024);
    const std::vector<PayloadExtent> samples{{source, 0, 64}, {source, 96, 16}};
    const auto plan = planPayloadLayout(samples, 16, 8);
    VectorSink sink(97);
    std::array<std::byte, 7> scratch;
    const auto result = copyPayload(plan, sink, scratch);
    require(result.ok() && result.completedBytes == 80, "copy includes every aligned candidate");
    require(source->maxRead == 7 && sink.maxWrite == 7, "scratch bounds I/O request sizes");
    require(sink.output[80] == std::byte{96} && sink.output[95] == std::byte{111}, "disjoint source mapping");

    VectorSink unused(97);
    require(copyPayload(plan, unused, {}).error == PayloadCopyError::EmptyScratch && unused.writes == 0,
            "empty scratch fails before I/O");
    require(copyPayload(planPayloadLayout({}, 16), unused, {}).ok(), "empty plan requires no buffer");

    source->readCalls = 0;
    source->failRead = 1;
    const auto readFailure = copyPayload(plan, unused, scratch);
    require(readFailure.error == PayloadCopyError::ReadFailed && readFailure.completedBytes == 7 && unused.writes == 1,
            "read error stops before writing failed/stale chunk");
    source->failRead = UINT64_MAX;
    VectorSink broken(97);
    broken.failWrite = 1;
    const auto writeFailure = copyPayload(plan, broken, scratch);
    require(writeFailure.error == PayloadCopyError::WriteFailed && writeFailure.completedBytes == 7 && broken.writes == 2,
            "write error reports only acknowledged prefix and stops");
    VectorSink retry(97);
    require(copyPayload(plan, retry, scratch).ok() && retry.output == sink.output,
            "same immutable plan can retry into a fresh destination");

    class PartialSink final : public PayloadCopySink {
    public:
        bool write(uint64_t, std::span<const std::byte> input) override {
            damaged.assign(input.begin(), input.begin() + 3);
            return false; // Simulate an OS write that changed only part of the chunk.
        }
        std::vector<std::byte> damaged;
    } partial;
    const auto partialFailure = copyPayload(plan, partial, scratch);
    require(partialFailure.error == PayloadCopyError::WriteFailed && partialFailure.completedBytes == 0 &&
            partial.damaged.size() == 3, "partial output is not acknowledged as successful or publishable");
}

} // namespace replay_tests
