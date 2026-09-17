#include "clipture/EncoderCadence.hpp"
#include <cstdlib>
#include <iostream>

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    for (int fps : {24, 30, 60, 120, 144, 210, 240}) {
        clipture::EncoderCadence clock(fps, 1'000'000);
        for (int i = 1; i <= fps * 120; ++i) {
            const int64_t expected = 1'000'000 + int64_t(i) * 10'000'000 / fps;
            require(clock.deadline100ns() == expected, "rational deadline drift");
            const auto tick = clock.advance(expected);
            require(tick.skipped == 0 && tick.lateness100ns == 0, "on-time tick skipped");
        }
        const auto due = clock.deadline100ns();
        const auto late = clock.advance(due + 750'000); // a 75 ms transition stall
        require(late.skipped > 0 && late.lateness100ns == 750'000, "late wake not counted");
        require(clock.deadline100ns() > due + 750'000, "catch-up burst left pending");
        require(late.deadline100ns <= due + 750'000, "future timestamp selected");
        require(due + 750'000 - late.deadline100ns < 10'000'000 / fps + 1, "video clock fell behind");
    }
    std::cout << "Encoder cadence: all rates, 120-second drift, late wake and no burst passed.\n";
}
