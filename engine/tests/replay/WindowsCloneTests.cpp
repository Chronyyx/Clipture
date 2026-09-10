#include "ExtentTestSupport.hpp"
#include "clipture/platform/windows/SealedPayloadFile.hpp"
#include "clipture/platform/windows/WindowsPayloadOutput.hpp"
#include <iostream>

namespace replay_tests {
using namespace clipture::replay;
using namespace clipture::platform::windows;

void verifyWindowsCloneFixture(const std::filesystem::path& sourcePath, const std::filesystem::path& outputPath) {
    const auto source = SealedPayloadFile::open(sourcePath);
    require(source != nullptr, "open immutable generated source file");
    const auto& info = source->cloneInfo();
    const uint64_t alignment = maximumCloneRequest(info.clusterBytes) ? info.clusterBytes : 4096;
    const std::vector<PayloadExtent> samples{{source, 0, source->size()}};
    const auto plan = planPayloadLayout(samples, 0, alignment);
    WindowsPayloadOutputFactory factory(outputPath);
    std::array<std::byte, 64 * 1024> scratch;
    const auto result = writePayloadFile(plan, factory, scratch);
    require(result.ok(), "Windows clone-or-copy file job succeeds");
    require(readFile(sourcePath) == readFile(outputPath), "Windows output byte parity");
    require(result.transfer.completedBytes + result.transfer.clonedBytes == source->size(), "Windows byte accounting");
    if (!info.refs || !info.refcounting) require(result.transfer.clonedBytes == 0, "unsupported volume cannot report cloning");
    std::cout << "Windows clone probe: queried=" << info.queried << " refs=" << info.refs
              << " refcounting=" << info.refcounting << " cluster=" << info.clusterBytes
              << " cloned=" << result.transfer.clonedBytes << " copied=" << result.transfer.completedBytes
              << " retry=" << result.retriedCopy << " nativeError=" << result.firstCloneError << '\n';
    if (!result.transfer.clonedBytes) std::cout << "SKIP: successful native ReFS clone not exercised; copy fallback verified.\n";
}

void testWindowsClone(const std::filesystem::path& parent) {
    ScratchDirectory directory(parent);
    const auto input = directory.path / "source.bin", output = directory.path / "output.bin";
    std::vector<char> expected(256 * 1024 + 17);
    for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<char>(i % 251);
    { std::ofstream file(input, std::ios::binary); file.write(expected.data(), expected.size()); require(file.good(), "write owned probe source"); }
    verifyWindowsCloneFixture(input, output);
    verifyNativeCloneRetry(input, directory.path / "retried.bin");
    {
        const auto sealed = SealedPayloadFile::open(input);
        require(sealed != nullptr && sealed->cloneInfo().queried, "real local file capability query");
        HANDLE writer = CreateFileW(input.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        require(writer == INVALID_HANDLE_VALUE, "sealed source prevents writes while pinned");
        const std::vector<PayloadExtent> samples{{sealed, 0, sealed->size()}};
        const auto plan = planPayloadLayout(samples, 0, 4096);
        WindowsPayloadOutputFactory collision(output);
        std::array<std::byte, 4096> scratch;
        require(writePayloadFile(plan, collision, scratch).error == PayloadJobError::CreateFailed && readFile(output) == expected,
                "CREATE_NEW collision cannot overwrite existing output");
        WindowsPayloadOutputFactory alias(input);
        require(writePayloadFile(plan, alias, scratch).error == PayloadJobError::CreateFailed, "same-path output cannot overwrite source");
    }
    { std::fstream file(input, std::ios::binary | std::ios::in | std::ios::out); file.write("changed", 7); require(file.good(), "modify unpinned owned source"); }
    require(readFile(output) == expected, "completed output independent of later source edits");
    require(std::filesystem::remove(input), "remove only owned probe source");
    require(readFile(output) == expected, "completed output survives source deletion");
    auto failing = std::make_shared<PatternSource>(8192); failing->failRead = 0;
    const std::vector<PayloadExtent> failedSample{{failing, 0, 8192}};
    const auto failedPath = directory.path / "failed.bin";
    WindowsPayloadOutputFactory failedFactory(failedPath);
    std::array<std::byte, 4096> scratch;
    const auto failed = writePayloadFile(planPayloadLayout(failedSample, 0, 4096), failedFactory, scratch);
    require(failed.error == PayloadJobError::TransferFailed && !std::filesystem::exists(failedPath),
            "failed attempt removed by owned handle, not left as a successful file");
    class ThrowingSource final : public PayloadExtentSource {
    public:
        uint64_t size() const noexcept override { return 8192; }
        bool read(uint64_t, std::span<std::byte>) const override { throw std::runtime_error("injected source exception"); }
    };
    const std::vector<PayloadExtent> throwingSample{{std::make_shared<ThrowingSource>(), 0, 8192}};
    requireThrows<std::runtime_error>([&] {
        writePayloadFile(planPayloadLayout(throwingSample, 0, 4096), failedFactory, scratch);
    });
    require(!std::filesystem::exists(failedPath), "exception unwinding discards the unfinished native file");
}
} // namespace replay_tests
