#include "clipture/platform/windows/InPlaceFile.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace clipture::platform::windows {
std::shared_ptr<InPlaceFile> InPlaceFile::create(const std::filesystem::path& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    std::shared_ptr<InPlaceFile> file;
    try {
        auto owner = std::unique_ptr<InPlaceFile>(new InPlaceFile(handle));
        handle = INVALID_HANDLE_VALUE;
        file = std::shared_ptr<InPlaceFile>(std::move(owner));
    } catch (...) { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); throw; }
    const std::array<std::byte, mediaStart> reserved{};
    if (!file->writeAt(0, reserved, file->io_.metadataWritten)) return {};
    return file;
}
InPlaceFile::~InPlaceFile() {
    if (discard_ && state_ != State::Published) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(handle_, FileDispositionInfo, &disposition, sizeof(disposition));
    }
    CloseHandle(handle_);
}
void InPlaceFile::discardWhenUnused() { std::lock_guard lock(mutex_); discard_ = true; }
void InPlaceFile::retainForRecovery() { std::lock_guard lock(mutex_); discard_ = false; }
uint64_t InPlaceFile::size() const noexcept { std::lock_guard lock(mutex_); return end_; }
InPlaceIo InPlaceFile::io() const { std::lock_guard lock(mutex_); return io_; }

bool InPlaceFile::writeAt(uint64_t offset, std::span<const std::byte> bytes, uint64_t& counter) {
    if (offset > INT64_MAX || bytes.size() > INT64_MAX - offset) return false;
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) return false;
    while (!bytes.empty()) {
        DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 512 * 1024)), written = 0;
        const bool ok = WriteFile(handle_, bytes.data(), count, &written, nullptr) != FALSE;
        counter += written;
        if (!ok || written != count) return false;
        bytes = bytes.subspan(count);
    }
    return true;
}
std::optional<uint64_t> InPlaceFile::append(std::span<const std::byte> bytes) {
    std::lock_guard lock(mutex_);
    if (state_ != State::Appending || bytes.empty()) return std::nullopt;
    const auto offset = end_;
    if (!writeAt(offset, bytes, io_.mediaWritten)) { state_ = State::Failed; return std::nullopt; }
    end_ += bytes.size();
    return offset;
}
bool InPlaceFile::read(uint64_t offset, std::span<std::byte> bytes) const {
    std::lock_guard lock(mutex_);
    // Only immutable committed media is exposed. Header/index patches cannot
    // invalidate any source extent supplied by this object.
    if (offset < mediaStart || offset > end_ || bytes.size() > end_ - offset) return false;
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) return false;
    while (!bytes.empty()) {
        DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 512 * 1024)), actual = 0;
        const bool ok = ReadFile(handle_, bytes.data(), count, &actual, nullptr) != FALSE;
        io_.bytesRead += actual;
        if (!ok || actual != count) return false;
        bytes = bytes.subspan(count);
    }
    return true;
}
bool InPlaceFile::writeSlot(uint64_t offset, std::span<const std::byte> bytes) {
    std::lock_guard lock(mutex_);
    if (state_ != State::Appending || offset < mediaStart || bytes.empty()) return false;
    if (!writeAt(offset, bytes, io_.mediaWritten)) { state_ = State::Failed; return false; }
    end_ = std::max(end_, offset + bytes.size());
    return true;
}
std::optional<uint64_t> InPlaceFile::appendFinalSample(std::span<const std::byte> bytes) {
    std::lock_guard lock(mutex_);
    if (state_ != State::Sealed || bytes.empty()) return std::nullopt;
    const auto offset = end_;
    if (!writeAt(offset, bytes, io_.mediaWritten)) { state_ = State::Failed; return std::nullopt; }
    end_ += bytes.size();
    return offset;
}
bool InPlaceFile::seal() {
    std::lock_guard lock(mutex_);
    if (state_ == State::Sealed) return true;
    if (state_ != State::Appending) return false;
    state_ = State::Sealed;
    return true;
}
bool InPlaceFile::finalize(std::span<const std::byte> prefix, std::span<const std::byte> movieIndex) {
    std::lock_guard lock(mutex_);
    if (state_ != State::Sealed || prefix.size() != mediaStart || movieIndex.empty()) return false;
    // Media range [mediaStart, end_) is NEVER rewritten during finalization.
    if (!writeAt(end_, movieIndex, io_.metadataWritten) || !writeAt(0, prefix, io_.metadataWritten) ||
        !FlushFileBuffers(handle_)) { state_ = State::Failed; return false; }
    state_ = State::Finalized;
    return true;
}
bool InPlaceFile::publish(const std::filesystem::path& destination) {
    std::lock_guard lock(mutex_);
    if (state_ != State::Finalized) return false;
    const auto name = std::filesystem::absolute(destination).wstring();
    if (name.size() > 32767) return false;
    const auto bytes = offsetof(FILE_RENAME_INFO, FileName) + (name.size() + 1) * sizeof(wchar_t);
    std::vector<std::byte> storage(bytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
    // Host publication may rename/unlink staging paths while packet leases keep
    // decoder preroll readable. Obtain the shareable reader before publishing,
    // so a reopen failure leaves the private finalized file available for retry.
    const auto reader = ReOpenFile(handle_, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (reader == INVALID_HANDLE_VALUE) return false;
    if (!SetFileInformationByHandle(handle_, FileRenameInfo, rename, static_cast<DWORD>(bytes))) {
        CloseHandle(reader);
        return false;
    }
    state_ = State::Published;
    CloseHandle(handle_);
    handle_ = reader;
    return true; // Same file identity; no copy fallback hidden here.
}
} // namespace clipture::platform::windows
