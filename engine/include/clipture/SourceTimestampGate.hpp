#pragma once
#include <cstdint>

namespace clipture {
// Source provenance, not output cadence. Never turn an old image into a new
// sample by replacing its timestamp with the current wall clock.
class SourceTimestampGate {
public:
    bool accept(int64_t timestamp100ns) {
        if (timestamp100ns <= 0 || timestamp100ns <= last_) return false;
        last_ = timestamp100ns;
        return true;
    }
    int64_t last() const { return last_; }
    void reset() { last_ = 0; }
private:
    int64_t last_ = 0;
};
}
