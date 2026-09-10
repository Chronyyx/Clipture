#pragma once
#include "clipture/replay/PayloadCloneJob.hpp"
#include <filesystem>

namespace clipture::platform::windows {
// Destination must be an authorized private staging name. CREATE_NEW never
// overwrites. No directories are created and no installed/user library is queried.
class WindowsPayloadOutputFactory final : public replay::PayloadOutputFactory {
public:
    explicit WindowsPayloadOutputFactory(std::filesystem::path path) : path_(std::move(path)) {}
    std::unique_ptr<replay::PayloadOutputAttempt> create(uint64_t outputSize) override;
    uint32_t lastError() const noexcept { return lastError_; }
private:
    std::filesystem::path path_;
    uint32_t lastError_ = 0;
};
} // namespace clipture::platform::windows
