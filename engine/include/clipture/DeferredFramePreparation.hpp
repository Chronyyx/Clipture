#pragma once

#include <functional>
#include <mutex>
#include <string>

namespace clipture {

// One preparation per captured image, shared by repeated cadence jobs. The
// callback owns immutable capture-time inputs and never runs on the scheduler.
class DeferredFramePreparation {
public:
    using Work = std::function<bool(std::string&)>;
    explicit DeferredFramePreparation(Work work) : work_(std::move(work)) {}
    bool prepare(std::string& error) {
        std::lock_guard lock(mutex_);
        if (!attempted_) {
            attempted_ = true;
            succeeded_ = work_(error_);
            work_ = {}; // Release HDR input/cursor snapshots once submitted.
        }
        error = error_;
        return succeeded_;
    }
private:
    std::mutex mutex_;
    Work work_;
    std::string error_;
    bool attempted_ = false;
    bool succeeded_ = false;
};
} // namespace clipture
