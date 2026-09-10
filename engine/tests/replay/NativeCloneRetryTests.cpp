#include "ExtentTestSupport.hpp"
#include "clipture/platform/windows/SealedPayloadFile.hpp"
#include "clipture/platform/windows/WindowsPayloadOutput.hpp"

namespace replay_tests {
namespace {
using namespace clipture::replay;
// Fault injection uses real private Windows output files but SIMULATES cloning
// using writes. It must never be counted as successful ReFS coverage.
class DamagedAttempt final : public PayloadOutputAttempt {
public:
    explicit DamagedAttempt(std::unique_ptr<PayloadOutputAttempt> output) : output_(std::move(output)) {}
    bool write(uint64_t offset, std::span<const std::byte> bytes) override { return output_->write(offset, bytes); }
    CloneOutcome clone(const PayloadExtent& input, uint64_t offset) override {
        if (++calls_ == 2) {
            const std::array<std::byte, 3> damage{std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
            require(output_->write(0, damage), "inject partial native attempt corruption");
            return {CloneStatus::Failed, ERROR_NOT_SUPPORTED};
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(input.length));
        require(input.source->read(input.offset, bytes) && output_->write(offset, bytes), "simulate first successful range");
        return {CloneStatus::Cloned, 0};
    }
    bool finish() override { return output_->finish(); }
    bool discard() override { return output_->discard(); }
private:
    std::unique_ptr<PayloadOutputAttempt> output_;
    int calls_ = 0;
};
class RetryFactory final : public PayloadOutputFactory {
public:
    explicit RetryFactory(const std::filesystem::path& path) : native_(path) {}
    std::unique_ptr<PayloadOutputAttempt> create(uint64_t bytes) override {
        auto output = native_.create(bytes);
        require(output != nullptr, "native CREATE_NEW succeeds only after prior damaged file was removed");
        if (++attempts == 1) return std::make_unique<DamagedAttempt>(std::move(output));
        return output;
    }
    int attempts = 0;
private:
    clipture::platform::windows::WindowsPayloadOutputFactory native_;
};
} // namespace

void verifyNativeCloneRetry(const std::filesystem::path& sourcePath, const std::filesystem::path& outputPath) {
    auto source = clipture::platform::windows::SealedPayloadFile::open(sourcePath);
    require(source && source->size() >= 196608, "bounded native retry fixture");
    const std::vector<PayloadExtent> samples{{source, 0, 65536}, {source, 131072, 65536}};
    const auto plan = planPayloadLayout(samples, 0, 4096);
    std::array<std::byte, 4096> scratch;
    RetryFactory factory(outputPath);
    const auto result = writePayloadFile(plan, factory, scratch);
    require(result.ok() && result.retriedCopy && result.firstCloneError == ERROR_NOT_SUPPORTED && factory.attempts == 2 &&
            result.transfer.clonedBytes == 0, "failed native attempt replaced with copy-only fresh file");
    const auto input = readFile(sourcePath), output = readFile(outputPath);
    require(output.size() == 131072 && std::equal(input.begin(), input.begin() + 65536, output.begin()) &&
            std::equal(input.begin() + 131072, input.begin() + 196608, output.begin() + 65536),
            "retry repairs the complete file, including corruption outside the failed range");
}
} // namespace replay_tests
