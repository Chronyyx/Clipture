#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace clipture {
// Bounded metadata only: no textures, leases, waits, or per-frame disk writes.
class FrameSelectionTrace {
public:
    struct Event {
        char kind = 'p'; // publication, tick, clear, unsupported consumer
        int64_t at = 0, source = 0, deadline = 0;
        uint64_t epoch = 0, sequence = 0;
        uint32_t depth = 0, coalesced = 0, overflow = 0, fps = 0;
    };
    struct Config { std::wstring path; uint32_t milliseconds = 90'000, limit = 65'536, queueCapacity = 8; };
    explicit FrameSelectionTrace(uint32_t queueCapacity);
    explicit FrameSelectionTrace(Config config);
    ~FrameSelectionTrace();
    bool enabled() const;
    void record(Event event) noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
