#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace clipture::relay {

// Line protocol between the engine and its elevated hotkey relay (ADR 0024).
// The engine sends "clipture-hotkey-relay/1 <accelerator>", then optional
// "accelerator <accelerator>" and "quit" lines; the relay answers "ok",
// "unsupported" or "rejected", then sends "hotkey" for each press. Bump the
// version whenever either side changes: an installed relay is a copy of an
// older engine and keeps running until the user allows it again.
inline constexpr std::string_view kHello = "clipture-hotkey-relay/1 ";
inline constexpr std::string_view kAcceleratorLine = "accelerator ";
// Accelerator changes the relay accepts at most this often.
inline constexpr int64_t kAcceleratorChangeInterval100ns = 50'000'000;  // 5 s

inline std::optional<std::string> helloAccelerator(std::string_view line) {
    if (line.substr(0, kHello.size()) != kHello) return std::nullopt;
    return std::string(line.substr(kHello.size()));
}

// The relay runs as administrator and sees keys pressed in admin windows, so it
// only ever reports a shortcut, never typing: the accelerator must hold Ctrl,
// Alt or Win, or be a key that types nothing (F1-F24, Insert, Pause...).
inline bool relaySafeAccelerator(std::string_view accelerator) {
    if (accelerator.empty() || accelerator.size() > 64) return false;
    std::string lower(accelerator);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool modifier = false;
    std::string key;
    std::size_t start = 0;
    while (start <= lower.size()) {
        const auto end = std::min(lower.find('+', start), lower.size());
        std::string token = lower.substr(start, end - start);
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        // The same names RawInputHotkey parses.
        if (token == "ctrl" || token == "control" || token == "cmdorctrl" || token == "commandorcontrol" ||
            token == "alt" || token == "option" || token == "super" || token == "meta" || token == "command" ||
            token == "cmd") {
            modifier = true;
        } else if (token != "shift") {
            if (!key.empty() || token.empty()) return false;  // one key, no empty parts
            key = token;
        }
        start = end + 1;
    }
    if (key.empty()) return false;
    if (modifier) return true;
    if (key.size() >= 2 && key[0] == 'f' && std::all_of(key.begin() + 1, key.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        const int number = std::stoi(key.substr(1));
        return number >= 1 && number <= 24;
    }
    for (const char* name : {"insert", "ins", "pause", "scrolllock", "printscreen", "home", "end", "pageup", "pagedown",
             "mediaplaypause", "medianexttrack", "mediaprevioustrack", "mediastop"}) {
        if (key == name) return true;
    }
    return false;
}

}  // namespace clipture::relay
