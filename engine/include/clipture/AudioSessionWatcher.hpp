#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace clipture {

// Keeps the set of processes with a sounding audio session on any active
// render device, driven by Windows session and device notifications. The
// callback runs on the watcher's thread whenever that set changes, so new
// sound gets a track without waiting for a polling pass. A slow rescan
// backs the notifications up.
class AudioSessionWatcher {
public:
    explicit AudioSessionWatcher(std::function<void()> onChange);
    ~AudioSessionWatcher();

    AudioSessionWatcher(const AudioSessionWatcher&) = delete;
    AudioSessionWatcher& operator=(const AudioSessionWatcher&) = delete;

    // False until the first scan finishes, and for good if registration failed.
    bool ready() const;
    std::vector<std::uint32_t> soundingProcessIds() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace clipture
