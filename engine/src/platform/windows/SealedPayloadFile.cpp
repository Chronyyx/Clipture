#include "clipture/platform/windows/SealedPayloadFile.hpp"
#include <algorithm>

namespace clipture::platform::windows {
SealedPayloadFile::SealedPayloadFile(HANDLE file, uint64_t size, CloneFileInfo info)
    : file_(file), size_(size), info_(std::move(info)) {}
SealedPayloadFile::~SealedPayloadFile() { CloseHandle(file_); }

std::shared_ptr<SealedPayloadFile> SealedPayloadFile::open(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        !GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(file);
        return {};
    }
    try {
        auto info = queryCloneFile(file);
        // Construct before shared_ptr allocation so either failure closes the handle once.
        auto owned = std::unique_ptr<SealedPayloadFile>(new SealedPayloadFile(file, static_cast<uint64_t>(size.QuadPart), std::move(info)));
        file = INVALID_HANDLE_VALUE;
        return std::shared_ptr<SealedPayloadFile>(std::move(owned));
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        throw;
    }
}

bool SealedPayloadFile::read(uint64_t offset, std::span<std::byte> destination) const {
    if (offset > size_ || destination.size() > size_ - offset) return false;
    if (destination.empty()) return true;
    std::lock_guard lock(readMutex_);
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file_, position, nullptr, FILE_BEGIN)) return false;
    while (!destination.empty()) {
        const auto count = static_cast<DWORD>(std::min<std::size_t>(destination.size(), 1024 * 1024));
        DWORD read = 0;
        if (!ReadFile(file_, destination.data(), count, &read, nullptr) || read != count) return false;
        destination = destination.subspan(count);
    }
    return true;
}
} // namespace clipture::platform::windows
