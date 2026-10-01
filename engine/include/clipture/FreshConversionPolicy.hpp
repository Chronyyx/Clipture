#pragma once
#include <cstdint>

namespace clipture {
// A first fresh image can bypass the canonical copy. Repeats materialize the
// independent canonical cache, never read an input currently mapped by NVENC.
class FreshConversionPolicy {
public:
    bool direct(bool enabled, uint64_t epoch, uint64_t sequence) const {
        return enabled && sequence != 0 && (epoch != epoch_ || sequence != sequence_);
    }
    void converted(uint64_t epoch, uint64_t sequence) { epoch_ = epoch; sequence_ = sequence; }
private:
    uint64_t epoch_ = 0;
    uint64_t sequence_ = 0;
};
}
