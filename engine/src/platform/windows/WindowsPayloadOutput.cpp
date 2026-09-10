#include "clipture/platform/windows/WindowsPayloadOutput.hpp"
#include "clipture/platform/windows/SealedPayloadFile.hpp"
#include <winioctl.h>
#include <algorithm>

namespace clipture::platform::windows {
namespace {
class WindowsPayloadOutput final : public replay::PayloadOutputAttempt {
public:
    explicit WindowsPayloadOutput(HANDLE file) : file_(file) {}
    ~WindowsPayloadOutput() override {
        discard();
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    bool initialize(uint64_t size) {
        if (size > INT64_MAX) return false;
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(size);
        if (!SetFilePointerEx(file_, end, nullptr, FILE_BEGIN) || !SetEndOfFile(file_)) return false;
        size_ = size;
        info_ = queryCloneFile(file_);
        return true;
    }
    bool write(uint64_t offset, std::span<const std::byte> bytes) override {
        if (file_ == INVALID_HANDLE_VALUE || offset > size_ || bytes.size() > size_ - offset) return false;
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file_, position, nullptr, FILE_BEGIN)) return false;
        while (!bytes.empty()) {
            const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1024 * 1024));
            DWORD written = 0;
            if (!WriteFile(file_, bytes.data(), count, &written, nullptr) || written != count) return false;
            bytes = bytes.subspan(count);
        }
        return true;
    }
    replay::CloneOutcome clone(const replay::PayloadExtent& input, uint64_t offset) override {
        const auto* source = dynamic_cast<const SealedPayloadFile*>(input.source.get());
        if (!source || file_ == INVALID_HANDLE_VALUE ||
            checkCloneRange(source->cloneInfo(), info_, input.offset, offset, input.length) != CloneEligibility::Eligible) return {};
        const auto limit = maximumCloneRequest(info_.clusterBytes);
        uint64_t done = 0;
        while (done < input.length) {
            const auto count = std::min(limit, input.length - done);
            DUPLICATE_EXTENTS_DATA request{};
            request.FileHandle = source->cloneHandle();
            request.SourceFileOffset.QuadPart = static_cast<LONGLONG>(input.offset + done);
            request.TargetFileOffset.QuadPart = static_cast<LONGLONG>(offset + done);
            request.ByteCount.QuadPart = static_cast<LONGLONG>(count);
            DWORD returned = 0;
            if (!DeviceIoControl(file_, FSCTL_DUPLICATE_EXTENTS_TO_FILE, &request, sizeof(request), nullptr, 0, &returned, nullptr)) {
                return {replay::CloneStatus::Failed, GetLastError()};
            }
            done += count;
        }
        return {replay::CloneStatus::Cloned, 0};
    }
    bool finish() override {
        if (file_ == INVALID_HANDLE_VALUE || !FlushFileBuffers(file_)) return false;
        if (!CloseHandle(file_)) return false;
        file_ = INVALID_HANDLE_VALUE; // Keep completed staging bytes. Not library publication.
        return true;
    }
    bool discard() override {
        if (file_ == INVALID_HANDLE_VALUE) return true;
        FILE_DISPOSITION_INFO disposition{TRUE};
        // Delete by owned handle, not a pathname that could have been replaced.
        if (!SetFileInformationByHandle(file_, FileDispositionInfo, &disposition, sizeof(disposition))) return false;
        if (!CloseHandle(file_)) return false;
        file_ = INVALID_HANDLE_VALUE;
        return true;
    }
private:
    HANDLE file_;
    uint64_t size_ = 0;
    CloneFileInfo info_;
};
} // namespace

std::unique_ptr<replay::PayloadOutputAttempt> WindowsPayloadOutputFactory::create(uint64_t outputSize) {
    lastError_ = 0;
    if (outputSize > INT64_MAX) { lastError_ = ERROR_FILE_TOO_LARGE; return {}; }
    HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { lastError_ = GetLastError(); return {}; }
    std::unique_ptr<WindowsPayloadOutput> output;
    try { output = std::make_unique<WindowsPayloadOutput>(file); }
    catch (...) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
        CloseHandle(file);
        throw;
    }
    if (!output->initialize(outputSize)) {
        lastError_ = GetLastError();
        return {};
    }
    return output;
}
} // namespace clipture::platform::windows
