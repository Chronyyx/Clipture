#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace clipture {

// Remembers one verdict per running process, so a question whose answer cannot
// change while the process lives (what its executable is) is asked once, not
// on every detection pass. A process is known by its ID, its parent's ID and
// its executable name. An entry not looked up during a pass is dropped when
// the pass ends, so an exited process leaves and a reused ID is asked again.
template <typename Verdict>
class ProcessVerdictCache {
public:
    void beginPass() {
        ++pass_;
    }

    // The remembered verdict, or null for a process not asked about yet.
    Verdict* find(std::uint32_t processId, std::uint32_t parentProcessId, std::string_view lowerExeName) {
        const auto found = entries_.find(processId);
        if (found == entries_.end()) return nullptr;
        if (found->second.parentProcessId != parentProcessId || found->second.lowerExeName != lowerExeName) {
            entries_.erase(found);
            return nullptr;
        }
        found->second.seenInPass = pass_;
        return &found->second.verdict;
    }

    void store(std::uint32_t processId, std::uint32_t parentProcessId, std::string_view lowerExeName, Verdict verdict) {
        entries_[processId] = Entry { parentProcessId, std::string(lowerExeName), pass_, std::move(verdict) };
    }

    void endPass() {
        std::erase_if(entries_, [this](const auto& item) { return item.second.seenInPass != pass_; });
    }

    std::size_t size() const {
        return entries_.size();
    }

private:
    struct Entry {
        std::uint32_t parentProcessId = 0;
        std::string lowerExeName;
        std::uint64_t seenInPass = 0;
        Verdict verdict {};
    };

    std::unordered_map<std::uint32_t, Entry> entries_;
    std::uint64_t pass_ = 0;
};

}  // namespace clipture
