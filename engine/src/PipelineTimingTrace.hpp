#pragma once
#include "clipture/MediaClock.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>

namespace clipture {
inline bool pipelineTimingEnabled() {
    static const bool enabled = [] {
        wchar_t value[2] {};
        return GetEnvironmentVariableW(L"CLIPTURE_PIPELINE_TRACE", value, 2) == 1 && value[0] == L'1';
    }();
    return enabled;
}

// Thread-confined, allocation-free accumulation; one small stderr record/sec.
// UTC is the observation interval, not a cross-device GPU-clock calibration.
class PipelineTimingTrace {
public:
    explicit PipelineTimingTrace(const char* stage, const char* kind = "cpu") : stage_(stage), kind_(kind) {}
    void record(int64_t duration100ns) {
        if (!pipelineTimingEnabled()) return;
        const auto now = monotonicNow100ns();
        if (!start_) start_ = now;
        const auto duration = std::max<int64_t>(0, duration100ns);
        ++count_; sum_ += duration; maximum_ = std::max(maximum_, duration);
        over1_ += duration > 10'000; over5_ += duration > 50'000; over10_ += duration > 100'000;
        if (now - start_ < 10'000'000) return;
        std::ostringstream out;
        out << "[pipeline-trace] {\"stage\":\"" << stage_ << "\",\"kind\":\"" << kind_
            << "\",\"utcMs\":" << (detail::preciseFileTime100ns() - 116444736000000000LL) / 10'000
            << ",\"windowMs\":" << (now - start_) / 10'000.0
            << ",\"count\":" << count_ << ",\"avgMs\":" << sum_ / (count_ * 10'000.0)
            << ",\"maxMs\":" << maximum_ / 10'000.0
            << ",\"over1Ms\":" << over1_ << ",\"over5Ms\":" << over5_
            << ",\"over10Ms\":" << over10_ << "}\n";
        std::cerr << out.str();
        start_ = now; count_ = sum_ = maximum_ = over1_ = over5_ = over10_ = 0;
    }
private:
    const char* stage_;
    const char* kind_;
    int64_t start_ = 0, count_ = 0, sum_ = 0, maximum_ = 0, over1_ = 0, over5_ = 0, over10_ = 0;
};
}
