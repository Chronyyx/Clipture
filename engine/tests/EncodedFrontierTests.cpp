#include "clipture/EncodedFrontier.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "Encoded frontier test failed: " << message << '\n';
    std::exit(1);
}
}

int main() {
    using namespace std::chrono_literals;
    clipture::EncodedFrontier frontier;
    require(!frontier.waitUntil(100, 5ms), "nothing encoded yet: the wait times out");
    frontier.advance(100);
    require(frontier.waitUntil(100, 0ms), "a frame ending at the horizon satisfies it at once");
    frontier.advance(50);
    require(frontier.waitUntil(100, 0ms), "an older frame never moves the frontier back");

    std::thread encoder([&] {
        std::this_thread::sleep_for(20ms);
        frontier.advance(150);
        std::this_thread::sleep_for(20ms);
        frontier.advance(250);
    });
    const auto started = std::chrono::steady_clock::now();
    require(frontier.waitUntil(200, 2s), "the save wakes when the frame arrives");
    require(std::chrono::steady_clock::now() - started < 1s, "and does not wait out the limit");
    encoder.join();
    std::cout << "Encoded frontier passed.\n";
    return 0;
}
