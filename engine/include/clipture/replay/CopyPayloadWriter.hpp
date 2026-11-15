#pragma once

#include "clipture/replay/PayloadLayoutPlan.hpp"
#include "clipture/replay/PayloadExtent.hpp"

namespace clipture::replay {
enum class CloneStatus { Unsupported, Cloned, Failed };
struct CloneOutcome {
    CloneStatus status = CloneStatus::Unsupported;
    uint32_t nativeError = 0;
};
class PayloadCloner {
public:
    virtual ~PayloadCloner() = default;
    // Unsupported MUST NOT modify the destination. Failed may have modified any
    // part of the range; the whole output attempt must then be discarded.
    virtual CloneOutcome clone(const PayloadExtent& source, uint64_t outputOffset) = 0;
};
} // namespace clipture::replay

namespace clipture::replay {

// Caller owns a private, unpublished destination, distinct from all sources.
// A false write may have partially modified it: discard/recover that output,
// never publish it. Flushing, durability and publication belong to the caller.
class PayloadCopySink {
public:
    virtual ~PayloadCopySink() = default;
    virtual bool write(uint64_t offset, std::span<const std::byte> bytes) = 0;
};

enum class PayloadCopyError { None, EmptyScratch, ReadFailed, WriteFailed, CloneFailed };
struct PayloadCopyResult {
    PayloadCopyError error = PayloadCopyError::None;
    uint64_t completedBytes = 0; // Only fully acknowledged writes.
    uint64_t paddingBytes = 0; // Acknowledged zero-filled gaps, separate from media.
    uint64_t clonedBytes = 0;
    uint64_t cloneAttempts = 0;
    uint64_t unsupportedRanges = 0;
    uint32_t nativeCloneError = 0;
    bool ok() const noexcept { return error == PayloadCopyError::None; }
};

// Copies ALL transfers and zero-fills placement gaps, including alignment
// candidates, with no clone calls or
// hidden payload allocation. Scratch bounds every read/write. I/O exceptions
// propagate; like an error result, they prohibit publishing the destination.
PayloadCopyResult copyPayload(const PayloadLayoutPlan& plan, PayloadCopySink& sink,
                              std::span<std::byte> scratch);

// Optional cloning only for geometric candidates. Unsupported ranges are copied;
// a failed clone invalidates the entire output, never patched in place.
PayloadCopyResult transferPayload(const PayloadLayoutPlan& plan, PayloadCopySink& sink,
                                  std::span<std::byte> scratch, PayloadCloner* cloner);

} // namespace clipture::replay
