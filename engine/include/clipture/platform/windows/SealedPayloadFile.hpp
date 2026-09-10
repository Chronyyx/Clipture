#pragma once
#include "clipture/platform/windows/CloneFileInfo.hpp"
#include "clipture/replay/PayloadExtent.hpp"
#include <filesystem>
#include <mutex>

namespace clipture::platform::windows {
// Experimental sealed-file adapter. Denies writes/deletion while pinned; does
// not reopen by path while copying/cloning. Not a live ReplaySegmentStore adapter.
class SealedPayloadFile final : public replay::PayloadExtentSource {
public:
    static std::shared_ptr<SealedPayloadFile> open(const std::filesystem::path& path);
    ~SealedPayloadFile() override;
    uint64_t size() const noexcept override { return size_; }
    bool read(uint64_t offset, std::span<std::byte> destination) const override;
    const CloneFileInfo& cloneInfo() const noexcept { return info_; }
    HANDLE cloneHandle() const noexcept { return file_; } // Borrowed, source pin must remain alive.
private:
    SealedPayloadFile(HANDLE file, uint64_t size, CloneFileInfo info);
    HANDLE file_;
    uint64_t size_;
    CloneFileInfo info_;
    mutable std::mutex readMutex_;
};
} // namespace clipture::platform::windows
