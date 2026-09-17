#pragma once

#include "clipture/MediaClock.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace clipture {

class DeadlineWait {
public:
    explicit DeadlineWait(int64_t spinTail100ns = 2'000) : timer_(CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)), spinTail100ns_(spinTail100ns) {}
    ~DeadlineWait() { if (timer_) CloseHandle(timer_); }
    DeadlineWait(const DeadlineWait&) = delete;
    DeadlineWait& operator=(const DeadlineWait&) = delete;

    template<typename KeepRunning>
    void until(int64_t deadline100ns, KeepRunning keepRunning) {
        while (keepRunning()) {
            const auto remaining = deadline100ns - monotonicNow100ns();
            if (remaining <= 0) return;
            // Encoder uses a 0.2 ms spin tail; idle capture uses no spin tail.
            if (remaining > spinTail100ns_) {
                LARGE_INTEGER due {};
                due.QuadPart = -(remaining - spinTail100ns_);
                if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer_, 100);
                } else {
                    std::this_thread::sleep_for(std::chrono::nanoseconds((remaining - spinTail100ns_) * 100));
                }
            } else {
                YieldProcessor();
            }
        }
    }
private:
    HANDLE timer_ = nullptr;
    int64_t spinTail100ns_;
};

} // namespace clipture
