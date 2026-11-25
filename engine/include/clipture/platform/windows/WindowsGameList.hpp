#pragma once

// The games Windows itself recognised (the Game Bar list in
// HKCU\System\GameConfigStore\Children), whatever launcher installed them.
// Windows adds a game when it first runs, so a game installed while Clipture
// records is known by the time it is played.
//
// Only entries Windows matched to its game database (they carry a TitleId)
// count: entries without one are apps someone marked by hand, such as Discord.
//
// Read once, then again only when Windows signals that the list changed:
// checking costs one non-blocking wait on an event.

#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

namespace clipture::platform {

class WindowsGameList {
public:
    // The Children key of the current user, or another key under HKCU (tests).
    explicit WindowsGameList(std::wstring childrenKey = L"System\\GameConfigStore\\Children");
    ~WindowsGameList();
    WindowsGameList(const WindowsGameList&) = delete;
    WindowsGameList& operator=(const WindowsGameList&) = delete;

    // Rereads the list when Windows changed it since the last read; true when
    // it did. Never blocks.
    bool refreshIfChanged();
    // Whether the executable at this full path (UTF-8) is a game Windows knows.
    bool contains(const std::string& processPath) const;
    std::size_t size() const;

private:
    void read();
    void watch();

    std::wstring childrenKey_;
    void* key_ = nullptr;      // HKEY
    void* changed_ = nullptr;  // event Windows signals on a change
    mutable std::mutex mutex_;
    std::shared_ptr<const std::unordered_set<std::string>> paths_;
};

}  // namespace clipture::platform
