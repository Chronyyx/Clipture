#pragma once

#include <windows.h>
#include <avrt.h>

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace clipture::stress {

// Normal-priority worker threads that burn CPU while active, standing in for a
// game's simulation and render threads (CS2 simulates smoke volumes on the CPU).
// Each thread is busy `busyPercent` of every millisecond. With `mmcssGames` the
// threads register as MMCSS "Games" at high priority, as game engines commonly do.
class CpuLoad {
public:
    CpuLoad(int threads, int busyPercent, bool mmcssGames = false)
        : busyPercent_(busyPercent), mmcssGames_(mmcssGames) {
        for (int i = 0; i < threads; ++i) workers_.emplace_back([this] { run(); });
    }
    ~CpuLoad() {
        stop_ = true;
        for (auto& worker : workers_) worker.join();
    }
    void setActive(bool active) { active_.store(active, std::memory_order_relaxed); }

private:
    void run() {
        DWORD taskIndex = 0;
        HANDLE task = mmcssGames_ ? AvSetMmThreadCharacteristicsW(L"Games", &taskIndex) : nullptr;
        if (task) AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
        LARGE_INTEGER frequency, start, now;
        QueryPerformanceFrequency(&frequency);
        volatile double sink = 0;
        while (!stop_) {
            if (!active_.load(std::memory_order_relaxed)) {
                Sleep(1);
                continue;
            }
            QueryPerformanceCounter(&start);
            const LONGLONG busyTicks = frequency.QuadPart * busyPercent_ / 100'000;
            do {
                for (int i = 0; i < 200; ++i) sink = sink + std::sqrt(static_cast<double>(i) + sink * 1e-9);
                QueryPerformanceCounter(&now);
            } while (now.QuadPart - start.QuadPart < busyTicks);
            if (busyPercent_ < 100) Sleep(1);
        }
        if (task) AvRevertMmThreadCharacteristics(task);
    }

    std::vector<std::thread> workers_;
    std::atomic<bool> active_{false};
    std::atomic<bool> stop_{false};
    int busyPercent_;
    bool mmcssGames_;
};

}  // namespace clipture::stress
