#pragma once
#include "clipture/replay/PayloadExtent.hpp"
#include "clipture/replay/InPlaceIo.hpp"
#include <Windows.h>
#include <filesystem>
#include <mutex>
#include <optional>

namespace clipture::platform::windows {
using replay::InPlaceIo;
// Owns ONE private file from initial append through metadata finalization and
// same-volume, no-replace rename. Failure preserves it for explicit recovery.
class InPlaceFile final : public replay::PayloadExtentSource {
public:
    static constexpr uint64_t mediaStart = 4096;
    static std::shared_ptr<InPlaceFile> create(const std::filesystem::path& path);
    ~InPlaceFile() override;
    uint64_t size() const noexcept override;
    bool read(uint64_t offset, std::span<std::byte> bytes) const override;
    std::optional<uint64_t> append(std::span<const std::byte> bytes);
    std::optional<uint64_t> appendFinalSample(std::span<const std::byte> bytes);
    bool writeSlot(uint64_t offset, std::span<const std::byte> bytes);
    void discardWhenUnused();
    void retainForRecovery();
    bool seal();
    bool finalize(std::span<const std::byte> prefix, std::span<const std::byte> movieIndex);
    bool publish(const std::filesystem::path& destination);
    InPlaceIo io() const;
private:
    explicit InPlaceFile(HANDLE handle) : handle_(handle) {}
    bool writeAt(uint64_t offset, std::span<const std::byte> bytes, uint64_t& counter);
    enum class State { Appending, Sealed, Finalized, Published, Failed };
    HANDLE handle_;
    mutable std::mutex mutex_;
    uint64_t end_ = mediaStart;
    State state_ = State::Appending;
    mutable InPlaceIo io_;
    bool discard_ = false;
};
} // namespace clipture::platform::windows
