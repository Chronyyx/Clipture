#include "clipture/FrameSelectionTrace.hpp"
#include "clipture/MediaClock.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace clipture {
namespace {
FrameSelectionTrace::Config environment(uint32_t capacity) {
    FrameSelectionTrace::Config config;
    config.queueCapacity = capacity;
    if (const auto path = _wgetenv(L"CLIPTURE_SELECTION_TRACE_PATH")) config.path = path;
    if (const auto seconds = _wgetenv(L"CLIPTURE_SELECTION_TRACE_SECONDS")) {
        wchar_t* end = nullptr;
        const long value = wcstol(seconds, &end, 10);
        if (end != seconds && !*end && value >= 10 && value <= 180) config.milliseconds = static_cast<uint32_t>(value * 1000);
    }
    return config;
}
}
struct FrameSelectionTrace::State {
    Config config;
    HANDLE file = INVALID_HANDLE_VALUE;
    int64_t start = monotonicNow100ns();
    int64_t utcMs = (mediaTimeFromSystemRelative100ns(start) - 116444736000000000LL) / 10000;
    std::atomic<bool> active = false;
    std::atomic<uint64_t> lost = 0;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
    std::vector<Event> events;
    std::thread writer;
    explicit State(Config next) : config(std::move(next)) {
        config.limit = std::clamp(config.limit, 1u, 65'536u);
        config.milliseconds = std::clamp(config.milliseconds, 1u, 180'000u);
        events.reserve(config.limit);
        // Never overwrite an earlier trial. Open once, serialize only after collection.
        file = CreateFileW(config.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { std::cerr << "[selection-trace] Cannot create new trace file.\n"; return; }
        active = true;
        try { writer = std::thread([this] { finish(); }); }
        catch (...) { active = false; CloseHandle(file); file = INVALID_HANDLE_VALUE; throw; }
    }
    ~State() {
        { std::lock_guard lock(mutex); stopping = true; }
        cv.notify_one();
        if (writer.joinable()) writer.join();
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    void finish() noexcept {
        try {
            std::unique_lock lock(mutex);
            const bool interrupted = cv.wait_for(lock, std::chrono::milliseconds(config.milliseconds), [this] { return stopping; });
            active = false;
            std::vector<Event> collected;
            collected.swap(events);
            const auto end = monotonicNow100ns();
            lock.unlock();
            std::ostringstream out;
            out << "# {\"version\":1,\"utcMs\":" << utcMs << ",\"duration100ns\":" << end - start
                << ",\"complete\":" << (interrupted ? "false" : "true") << ",\"lost\":" << lost.load()
                << ",\"limit\":" << config.limit << ",\"queueCapacity\":" << config.queueCapacity << "}\n"
                << "kind,at,source,deadline,epoch,sequence,depth,coalesced,overflow,fps\n";
            for (const auto& e : collected) out << e.kind << ',' << e.at - start << ','
                << (e.source ? e.source - start : 0) << ',' << (e.deadline ? e.deadline - start : 0) << ','
                << e.epoch << ',' << e.sequence << ',' << e.depth << ',' << e.coalesced << ',' << e.overflow << ',' << e.fps << '\n';
            const auto bytes = out.str();
            DWORD written = 0;
            if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size())
                std::cerr << "[selection-trace] Write failed; reject this trace.\n";
            else std::cerr << "[selection-trace] Finished metadata collection: events=" << collected.size()
                << " lost=" << lost.load() << " complete=" << !interrupted << '\n';
        } catch (...) { std::cerr << "[selection-trace] Serialization failed; reject this trace.\n"; }
    }
};
FrameSelectionTrace::FrameSelectionTrace(uint32_t capacity) : FrameSelectionTrace(environment(capacity)) {}
FrameSelectionTrace::FrameSelectionTrace(Config config) {
    if (!config.path.empty()) {
        try { state_ = std::make_unique<State>(std::move(config)); }
        catch (...) { std::cerr << "[selection-trace] Disabled after initialization failure.\n"; }
    }
}
FrameSelectionTrace::~FrameSelectionTrace() = default;
bool FrameSelectionTrace::enabled() const { return state_ && state_->active.load(std::memory_order_relaxed); }
void FrameSelectionTrace::record(Event event) noexcept {
    if (!enabled()) return;
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock) { ++state_->lost; return; }
    if (!state_->active) return;
    if (state_->events.size() == state_->config.limit) { ++state_->lost; return; }
    event.at = monotonicNow100ns();
    state_->events.push_back(event); // preallocated; no resource references retained
}
}
