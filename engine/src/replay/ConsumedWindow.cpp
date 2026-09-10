#include "clipture/replay/ConsumedWindow.hpp"

namespace clipture::replay {
std::vector<EncodedPacket> selectReplayVideo(std::vector<EncodedPacket> video,
    int durationSeconds, int64_t horizon, int64_t consumedEnd) {
    std::erase_if(video, [](const auto& packet) { return packet.kind != PacketKind::Video || payloadEmpty(packet); });
    if (video.empty()) return {};
    const auto newestDuration = std::max<int64_t>(1, video.back().duration100ns);
    const auto end = std::max(horizon, video.back().pts100ns + newestDuration);
    if (consumedEnd > 0 && end <= consumedEnd) return {};
    const auto start = std::max<int64_t>(consumedEnd, end - static_cast<int64_t>(durationSeconds) * 10'000'000LL);
    auto first = std::lower_bound(video.begin(), video.end(), start,
        [](const auto& packet, int64_t pts) { return packet.pts100ns < pts; });
    if (first == video.end()) first = std::prev(video.end());
    auto keyframe = first;
    while (keyframe != video.begin() && !keyframe->keyframe) --keyframe;
    if (!keyframe->keyframe) {
        keyframe = std::find_if(first, video.end(), [](const auto& packet) { return packet.keyframe; });
        if (keyframe == video.end()) return {};
    }
    video.erase(video.begin(), keyframe);
    video.back().duration100ns = std::max(newestDuration, end - video.back().pts100ns);
    return video;
}
}
