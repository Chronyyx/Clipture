#pragma once

#include <algorithm>
#include <cstddef>

namespace clipture {

constexpr std::size_t boundedWriteSize(std::size_t remaining, std::size_t maximum) {
    return maximum == 0 ? 0 : std::min(remaining, maximum);
}

}  // namespace clipture
