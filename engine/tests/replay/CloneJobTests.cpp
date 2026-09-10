#include "ExtentTestSupport.hpp"
#include "clipture/replay/PayloadCloneJob.hpp"

namespace replay_tests {
namespace {
using namespace clipture::replay;
enum class Behavior { Clone, Unsupported, PartialFailure, WriteFailure, FinishFailure, CleanupFailure };
struct Record {
    std::vector<std::byte> bytes;
    int cloneCalls = 0;
    bool discarded = false, finished = false;
};
class Attempt final : public PayloadOutputAttempt {
public:
    Attempt(std::shared_ptr<Record> record, Behavior behavior) : record_(std::move(record)), behavior_(behavior) {}
    bool write(uint64_t offset, std::span<const std::byte> bytes) override {
        if (behavior_ == Behavior::WriteFailure) return false;
        if (offset > record_->bytes.size() || bytes.size() > record_->bytes.size() - offset) return false;
        std::copy(bytes.begin(), bytes.end(), record_->bytes.begin() + static_cast<std::size_t>(offset));
        return true;
    }
    CloneOutcome clone(const PayloadExtent& input, uint64_t offset) override {
        ++record_->cloneCalls;
        if (behavior_ == Behavior::Unsupported || behavior_ == Behavior::WriteFailure) return {};
        if ((behavior_ == Behavior::PartialFailure || behavior_ == Behavior::CleanupFailure) && record_->cloneCalls == 2) {
            std::fill(record_->bytes.begin(), record_->bytes.end(), std::byte{0xa5}); // Damaged attempt, including prior successful range.
            return {CloneStatus::Failed, 1234};
        }
        require(input.source->read(input.offset, std::span(record_->bytes).subspan(static_cast<std::size_t>(offset),
                                                                                static_cast<std::size_t>(input.length))), "fake clone read");
        return {CloneStatus::Cloned, 0};
    }
    bool finish() override { record_->finished = behavior_ != Behavior::FinishFailure; return record_->finished; }
    bool discard() override { record_->discarded = behavior_ != Behavior::CleanupFailure; return record_->discarded; }
private:
    std::shared_ptr<Record> record_;
    Behavior behavior_;
};
class Factory final : public PayloadOutputFactory {
public:
    explicit Factory(Behavior behavior) : behavior(behavior) {}
    std::unique_ptr<PayloadOutputAttempt> create(uint64_t size) override {
        if (failCreate) return {};
        if (!records.empty()) require(records.back()->discarded, "failed attempt discarded before replacement creation");
        auto record = std::make_shared<Record>();
        record->bytes.resize(static_cast<std::size_t>(size), std::byte{0xff});
        records.push_back(record);
        return std::make_unique<Attempt>(record, records.size() == 1 ? behavior : Behavior::Clone);
    }
    Behavior behavior;
    bool failCreate = false;
    std::vector<std::shared_ptr<Record>> records;
};
} // namespace

void testCloneJob() {
    const auto source = std::make_shared<PatternSource>(256);
    const std::vector<PayloadExtent> samples{{source, 0, 16}, {source, 32, 19}};
    const auto plan = planPayloadLayout(samples, 3, 8, PayloadPlacement::AlignRegions);
    std::array<std::byte, 7> scratch;
    VectorSink expected(static_cast<std::size_t>(plan.outputEnd()));
    require(copyPayload(plan, expected, scratch).ok(), "reference copy");
    for (const auto behavior : {Behavior::Clone, Behavior::Unsupported, Behavior::PartialFailure}) {
        Factory factory(behavior);
        const auto result = writePayloadFile(plan, factory, scratch);
        require(result.ok() && factory.records.back()->finished && factory.records.back()->bytes == expected.output,
                "clone/copy/retry produce identical final payload and padding");
        require(result.transfer.completedBytes + result.transfer.clonedBytes == plan.payloadBytes(), "media accounting");
        if (behavior == Behavior::Clone) require(result.transfer.clonedBytes == 32 && factory.records.size() == 1, "successful candidates count as cloned");
        if (behavior == Behavior::Unsupported) require(result.transfer.unsupportedRanges == 2 && !result.retriedCopy, "unsupported falls back without clone mutation");
        if (behavior == Behavior::PartialFailure) {
            require(result.retriedCopy && result.firstCloneError == 1234 && factory.records.size() == 2 &&
                    factory.records[0]->discarded && !factory.records[0]->finished && factory.records[1]->cloneCalls == 0 &&
                    result.transfer.clonedBytes == 0, "partial clone invalidates full attempt and fresh retry never clones");
        }
    }
    for (const auto behavior : {Behavior::WriteFailure, Behavior::FinishFailure, Behavior::CleanupFailure}) {
        Factory factory(behavior);
        const auto result = writePayloadFile(plan, factory, scratch);
        require(!result.ok() && factory.records.size() == 1 && !factory.records[0]->finished, "failed write/finish/cleanup never publishes or retries");
        if (behavior == Behavior::CleanupFailure) require(result.error == PayloadJobError::CleanupFailed, "cleanup failure is explicit");
    }
    Factory missing(Behavior::Clone); missing.failCreate = true;
    require(writePayloadFile(plan, missing, scratch).error == PayloadJobError::CreateFailed, "creation failure");
}
} // namespace replay_tests
