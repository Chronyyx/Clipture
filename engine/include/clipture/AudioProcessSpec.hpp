#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace clipture {

enum class AudioProcessKind {
    App,
    Game
};

struct AudioProcessSpec {
    AudioProcessKind kind = AudioProcessKind::App;
    std::string processName;
    uint32_t processId = 0;
    bool pidSpecific = false;
};

inline AudioProcessSpec parseAudioProcessSpec(std::string_view sourceSpec) {
    const auto parsePidSpec = [&](std::string_view prefix, AudioProcessKind kind) -> AudioProcessSpec {
        const auto pidEnd = sourceSpec.find(':', prefix.size());
        if (pidEnd == std::string_view::npos) return { kind, std::string(sourceSpec.substr(prefix.size())), 0, false };

        uint32_t processId = 0;
        const auto pidText = sourceSpec.substr(prefix.size(), pidEnd - prefix.size());
        const auto [end, error] = std::from_chars(pidText.data(), pidText.data() + pidText.size(), processId);
        if (error != std::errc {} || end != pidText.data() + pidText.size() || processId == 0) {
            return { kind, std::string(sourceSpec.substr(pidEnd + 1)), 0, false };
        }
        return { kind, std::string(sourceSpec.substr(pidEnd + 1)), processId, true };
    };

    if (sourceSpec.starts_with("app-pid:")) return parsePidSpec("app-pid:", AudioProcessKind::App);
    if (sourceSpec.starts_with("game-pid:")) return parsePidSpec("game-pid:", AudioProcessKind::Game);
    if (sourceSpec.starts_with("app:")) {
        return { AudioProcessKind::App, std::string(sourceSpec.substr(4)), 0, false };
    }
    if (sourceSpec.starts_with("game:")) {
        return { AudioProcessKind::Game, std::string(sourceSpec.substr(5)), 0, false };
    }
    return { AudioProcessKind::App, std::string(sourceSpec), 0, false };
}

inline std::string audioProcessName(std::string_view sourceSpec) {
    return parseAudioProcessSpec(sourceSpec).processName;
}

inline std::string audioProcessSourceId(std::string_view sourceSpec) {
    const auto parsed = parseAudioProcessSpec(sourceSpec);
    return std::string(parsed.kind == AudioProcessKind::Game ? "game:" : "app:") + parsed.processName;
}

inline bool isPidAudioProcessSpec(std::string_view sourceSpec) {
    return parseAudioProcessSpec(sourceSpec).pidSpecific;
}

inline uint32_t audioProcessId(std::string_view sourceSpec) {
    return parseAudioProcessSpec(sourceSpec).processId;
}

inline std::string makePidAudioProcessSpec(
    AudioProcessKind kind,
    uint32_t processId,
    std::string_view processName) {
    if (processId == 0 || processName.empty()) return {};
    return std::string(kind == AudioProcessKind::Game ? "game-pid:" : "app-pid:") +
        std::to_string(processId) + ":" + std::string(processName);
}

}  // namespace clipture
