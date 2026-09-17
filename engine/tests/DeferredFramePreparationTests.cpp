#include "clipture/DeferredFramePreparation.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

static void require(bool value) { if (!value) std::abort(); }
int main() {
    std::atomic<int> runs = 0;
    auto source = std::make_shared<int>(42);
    std::weak_ptr<int> weak = source;
    auto preparation = std::make_shared<clipture::DeferredFramePreparation>(
        [source, &runs](std::string&) { ++runs; return *source == 42; });
    source.reset();
    require(!weak.expired());
    std::vector<std::thread> jobs;
    for (int i = 0; i < 16; ++i) jobs.emplace_back([preparation] {
        std::string error;
        require(preparation->prepare(error));
        require(error.empty());
    });
    for (auto& job : jobs) job.join();
    require(runs == 1);
    require(weak.expired());
    clipture::DeferredFramePreparation failure([&](std::string& error) {
        ++runs; error = "device removed"; return false;
    });
    for (int i = 0; i < 3; ++i) {
        std::string error;
        require(!failure.prepare(error) && error == "device removed");
    }
    require(runs == 2);
    { clipture::DeferredFramePreparation discarded([&](std::string&) { ++runs; return true; }); }
    require(runs == 2); // Unselected/coalesced frames do no GPU work.
    std::cout << "Deferred frame preparation: once-only, concurrent repeats, failure and discard passed.\n";
}
