#pragma once

#include "TestSupport.hpp"
#include "clipture/replay/CopyPayloadWriter.hpp"

#include <limits>

namespace replay_tests {

class PatternSource final : public clipture::replay::PayloadExtentSource {
public:
    explicit PatternSource(uint64_t length) : length_(length) {}
    uint64_t size() const noexcept override { return length_; }
    bool read(uint64_t offset, std::span<std::byte> destination) const override {
        maxRead = std::max(maxRead, destination.size());
        if (readCalls++ == failRead || offset > length_ || destination.size() > length_ - offset) return false;
        for (std::size_t i = 0; i < destination.size(); ++i) {
            destination[i] = static_cast<std::byte>((offset + i) % 251);
        }
        return true;
    }
    mutable std::size_t maxRead = 0;
    mutable uint64_t readCalls = 0;
    uint64_t failRead = std::numeric_limits<uint64_t>::max();
private:
    uint64_t length_;
};

class VectorSink final : public clipture::replay::PayloadCopySink {
public:
    explicit VectorSink(std::size_t size) : output(size, std::byte{0xff}) {}
    bool write(uint64_t offset, std::span<const std::byte> input) override {
        maxWrite = std::max(maxWrite, input.size());
        if (writes++ == failWrite) return false;
        if (offset > output.size() || input.size() > output.size() - offset) return false;
        std::copy(input.begin(), input.end(), output.begin() + static_cast<std::size_t>(offset));
        return true;
    }
    std::vector<std::byte> output;
    std::size_t maxWrite = 0;
    uint64_t writes = 0;
    uint64_t failWrite = std::numeric_limits<uint64_t>::max();
};

template<class Exception, class Action>
void requireThrows(Action action) {
    try { action(); } catch (const Exception&) { return; }
    throw std::runtime_error("Expected payload validation exception");
}

} // namespace replay_tests
