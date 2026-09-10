#pragma once

#include <cstddef>
#include <vector>

namespace clipture::replay {

// Small, immutable out-of-band parameters retained after Annex B framing is
// removed. No payload offsets: this survives both RAM and disk-backed storage.
struct H264DecoderConfig {
    std::vector<std::byte> sps;
    std::vector<std::byte> pps;
};

}  // namespace clipture::replay
