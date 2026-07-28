#pragma once

#include <Windows.h>

#include <cstdint>

namespace clipture {

namespace detail {

inline int64_t qpcTicksTo100ns(int64_t ticks, int64_t frequency) {
    if (frequency <= 0) return 0;
    const int64_t wholeSeconds = ticks / frequency;
    const int64_t remainder = ticks % frequency;
    return wholeSeconds * 10'000'000LL + (remainder * 10'000'000LL) / frequency;
}

inline int64_t queryPerformanceTime100ns() {
    LARGE_INTEGER counter {};
    QueryPerformanceCounter(&counter);
    static const int64_t frequency = [] {
        LARGE_INTEGER value {};
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return qpcTicksTo100ns(counter.QuadPart, frequency);
}

inline int64_t preciseFileTime100ns() {
    FILETIME fileTime {};
    GetSystemTimePreciseAsFileTime(&fileTime);
    ULARGE_INTEGER value {};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return static_cast<int64_t>(value.QuadPart);
}

struct MediaClockAnchor {
    int64_t systemRelative100ns = 0;
    int64_t fileTime100ns = 0;
};

inline const MediaClockAnchor& mediaClockAnchor() {
    static const MediaClockAnchor anchor {
        queryPerformanceTime100ns(),
        preciseFileTime100ns(),
    };
    return anchor;
}

}  // namespace detail

// WGC SystemRelativeTime and WASAPI's QPC position are both expressed in
// 100-nanosecond units. Keep packets on the existing absolute FILETIME-based
// timeline while deriving them from the monotonic QPC clock.
inline int64_t mediaTimeFromSystemRelative100ns(int64_t systemRelative100ns) {
    const auto& anchor = detail::mediaClockAnchor();
    return anchor.fileTime100ns + (systemRelative100ns - anchor.systemRelative100ns);
}

inline int64_t monotonicNow100ns() {
    return detail::queryPerformanceTime100ns();
}

inline int64_t mediaNow100ns() {
    return mediaTimeFromSystemRelative100ns(monotonicNow100ns());
}

}  // namespace clipture
