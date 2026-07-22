#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace clipture {

struct AacEncodedFrame {
    std::vector<std::byte> payload;
    int64_t pts100ns = 0;
    uint32_t durationFrames = 1024;
    int32_t primingFrames = 0;
};

class AacEncoderSession {
public:
    AacEncoderSession();
    ~AacEncoderSession();

    AacEncoderSession(const AacEncoderSession&) = delete;
    AacEncoderSession& operator=(const AacEncoderSession&) = delete;

    bool start(int sampleRate, int channels, std::string& error);
    bool encode(
        std::span<const std::byte> pcm,
        int64_t pts100ns,
        uint32_t frameCount,
        std::vector<AacEncodedFrame>& output,
        std::string& error);
    bool finish(std::vector<AacEncodedFrame>& output, std::string& error);
    void reset();

    bool active() const;
    int sampleRate() const;
    int channels() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipture
