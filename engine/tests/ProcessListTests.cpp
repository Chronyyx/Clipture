// The process list the detection loop takes each pass, the feed that shares
// it with audio capture threads, and the per-process verdict memory.
#include "clipture/ProcessSnapshotFeed.hpp"
#include "clipture/ProcessVerdictCache.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

using namespace clipture;

namespace {

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

// The one-call list has to say what Toolhelp says: same processes, parents
// and names, in the same order. Processes that start or exit between the two
// reads are allowed for.
void systemListMatchesToolhelp() {
    RunningProcessSnapshot fast;
    require(RunningProcessSnapshot::captureFromSystemList(fast), "the one-call process list should be available");
    const auto reference = RunningProcessSnapshot::captureFromToolhelp();
    require(fast.valid() && reference.valid(), "both process lists should be readable");

    const auto milliseconds = [](auto&& capture) {
        const auto startedAt = std::chrono::steady_clock::now();
        for (int run = 0; run < 5; ++run) capture();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt).count() / 5;
    };
    std::cout << "one call: " << milliseconds([] { RunningProcessSnapshot::captureNameOnly(); })
              << " ms, Toolhelp: " << milliseconds([] { RunningProcessSnapshot::captureFromToolhelp(); })
              << " ms for " << fast.size() << " processes" << std::endl;
    require(fast.size() > 4, "the process list should not be nearly empty");

    std::map<DWORD, const RunningProcessInfo*> referenceById;
    for (const auto& entry : reference.entries()) referenceById[entry.processId] = &entry;
    std::size_t common = 0;
    for (const auto& entry : fast.entries()) {
        const auto found = referenceById.find(entry.processId);
        if (found == referenceById.end()) continue;
        ++common;
        require(found->second->exeName == entry.exeName,
            "name differs for process " + std::to_string(entry.processId) + ": '" + entry.exeName + "' vs '" +
                found->second->exeName + "'");
        require(found->second->parentProcessId == entry.parentProcessId,
            "parent differs for process " + std::to_string(entry.processId));
        require(found->second->lowerExeName == entry.lowerExeName, "lower-case name differs");
    }
    require(common * 10 >= reference.size() * 9, "the two lists should name nearly the same processes");

    // Same order, comparing the processes both lists hold.
    std::vector<DWORD> fastOrder;
    std::map<DWORD, bool> inFast;
    for (const auto& entry : fast.entries()) inFast[entry.processId] = true;
    for (const auto& entry : fast.entries()) {
        if (referenceById.contains(entry.processId)) fastOrder.push_back(entry.processId);
    }
    std::vector<DWORD> referenceOrder;
    for (const auto& entry : reference.entries()) {
        if (inFast.contains(entry.processId)) referenceOrder.push_back(entry.processId);
    }
    require(fastOrder == referenceOrder, "the one-call list should keep Toolhelp's order");

    const auto* self = findProcessInfo(fast, GetCurrentProcessId());
    require(self != nullptr, "the list should include this process");
    const auto path = RunningProcessSnapshot::queryProcessPath(GetCurrentProcessId());
    require(path.size() > self->exeName.size() &&
            path.compare(path.size() - self->exeName.size(), self->exeName.size(), self->exeName) == 0,
        "this process's path should end with its listed name");
    require(RunningProcessSnapshot::queryProcessPath(0).empty(), "process 0 has no path");
}

void verdictsLastForTheProcessLifetime() {
    ProcessVerdictCache<int> verdicts;
    verdicts.beginPass();
    require(verdicts.find(100, 4, "game.exe") == nullptr, "an unseen process has no verdict");
    verdicts.store(100, 4, "game.exe", 7);
    verdicts.store(200, 4, "tool.exe", 9);
    verdicts.endPass();
    require(verdicts.size() == 2, "verdicts stored during a pass survive it");

    verdicts.beginPass();
    const auto* kept = verdicts.find(100, 4, "game.exe");
    require(kept && *kept == 7, "a running process keeps its verdict");
    verdicts.endPass();
    require(verdicts.size() == 1, "a process not looked up in a pass has exited and is forgotten");

    // The ID came back as another program, or under another parent.
    verdicts.beginPass();
    require(verdicts.find(100, 4, "other.exe") == nullptr, "a reused ID with another name is asked again");
    verdicts.store(100, 4, "other.exe", 1);
    require(verdicts.find(100, 8, "other.exe") == nullptr, "a reused ID with another parent is asked again");
    verdicts.endPass();
    require(verdicts.size() == 0, "a mismatched entry is dropped");

    // A verdict can be revised in place (a path that could not be read yet).
    verdicts.beginPass();
    verdicts.store(300, 4, "late.exe", 0);
    if (auto* verdict = verdicts.find(300, 4, "late.exe")) *verdict = 5;
    verdicts.endPass();
    verdicts.beginPass();
    const auto* revised = verdicts.find(300, 4, "late.exe");
    require(revised && *revised == 5, "a revised verdict is kept");
    verdicts.endPass();
}

void feedHandsOutOnlyNewerLists() {
    ProcessSnapshotFeed feed;
    const auto never = [] { return false; };
    std::uint64_t seen = feed.generation();
    require(feed.waitForNewer(seen, std::chrono::milliseconds(20), never) == nullptr,
        "nothing published: the wait times out");

    const auto first = feed.publish(RunningProcessSnapshot::captureNameOnly());
    const auto read = feed.waitForNewer(seen, std::chrono::milliseconds(20), never);
    require(read == first, "a list published after the reader's mark is handed out");
    require(feed.waitForNewer(seen, std::chrono::milliseconds(20), never) == nullptr,
        "the same list is not handed out twice");

    // A waiting reader gets the next list as it is published.
    std::atomic<bool> got { false };
    std::thread reader([&] {
        got = feed.waitForNewer(seen, std::chrono::seconds(5), never) != nullptr;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    feed.publish(RunningProcessSnapshot::captureNameOnly());
    reader.join();
    require(got, "a waiting reader receives the next published list");

    // A cancelled reader returns at once, without a list.
    std::atomic<bool> cancelled { false };
    const auto startedAt = std::chrono::steady_clock::now();
    std::thread stopped([&] {
        feed.waitForNewer(seen, std::chrono::seconds(5), [&] { return cancelled.load(); });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cancelled = true;
    feed.wake();
    stopped.join();
    require(std::chrono::steady_clock::now() - startedAt < std::chrono::seconds(2),
        "wake() ends a cancelled wait early");
}

}  // namespace

int main() {
    try {
        systemListMatchesToolhelp();
        verdictsLastForTheProcessLifetime();
        feedHandsOutOnlyNewerLists();
        std::cout << "PASS: process list" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
