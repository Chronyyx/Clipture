#pragma once
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
