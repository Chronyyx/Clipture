#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace clipture::replay {

// A pin on append-only storage, not a pathname. Committed bytes never change
// while a reference exists. Appending may increase size(), but cannot invalidate
// existing ranges. Implementations own reclamation and thread-safe reads.
class PayloadExtentSource {
public:
    virtual ~PayloadExtentSource() = default;
    virtual uint64_t size() const noexcept = 0;
    virtual bool read(uint64_t offset, std::span<std::byte> destination) const = 0;
};

struct PayloadExtent {
    std::shared_ptr<const PayloadExtentSource> source;
    uint64_t offset = 0;
    uint64_t length = 0;
};

} // namespace clipture::replay
