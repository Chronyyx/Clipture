#pragma once

#include "clipture/replay/PayloadExtent.hpp"

#include <vector>

namespace clipture::replay {

// AlignmentCandidate is geometric only: NOT permission/support for cloning.
// A later backend must check volume, handles, integrity and operation limits.
enum class PayloadTransferKind { Copy, AlignmentCandidate };
enum class PayloadPlacement { Compact, AlignRegions };

struct PayloadTransfer {
    PayloadExtent input;
    uint64_t outputOffset = 0;
    PayloadTransferKind kind = PayloadTransferKind::Copy;
};

class PayloadLayoutPlan {
public:
    const std::vector<PayloadTransfer>& transfers() const noexcept { return transfers_; }
    const std::vector<uint64_t>& sampleOffsets() const noexcept { return sampleOffsets_; }
    uint64_t outputStart() const noexcept { return outputStart_; }
    uint64_t outputEnd() const noexcept { return outputEnd_; }
    uint64_t payloadBytes() const noexcept { return outputEnd_ - outputStart_ - paddingBytes_; }
    uint64_t paddingBytes() const noexcept { return paddingBytes_; }
    uint64_t alignmentCandidateBytes() const noexcept { return candidateBytes_; }

private:
    friend PayloadLayoutPlan planPayloadLayout(std::span<const PayloadExtent>, uint64_t, uint64_t, PayloadPlacement);
    std::vector<PayloadTransfer> transfers_;
    std::vector<uint64_t> sampleOffsets_;
    uint64_t outputStart_ = 0;
    uint64_t outputEnd_ = 0;
    uint64_t candidateBytes_ = 0;
    uint64_t paddingBytes_ = 0;
};

// One nonempty extent per already-formatted sample, in output order. No I/O,
// codec conversion or MP4 metadata generation. Adjacent source ranges
// coalesce without erasing sample boundaries. alignment=0 disables candidates;
// otherwise it must be a power of two. AlignRegions requires nonzero alignment
// and may insert < alignment zero bytes before a contiguous source run, never
// inside a sample. It pads only if candidate body bytes exceed the gap cost.
// Invalid ranges/overflow throw before I/O.
PayloadLayoutPlan planPayloadLayout(std::span<const PayloadExtent> samples,
                                    uint64_t outputStart, uint64_t alignment = 0,
                                    PayloadPlacement placement = PayloadPlacement::Compact);

} // namespace clipture::replay
