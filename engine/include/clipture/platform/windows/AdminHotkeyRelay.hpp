#pragma once

#include <functional>
#include <memory>
#include <string>

namespace clipture::platform {

// Hotkey in admin games (ADR 0024). Windows keeps raw input from a game that
// runs as administrator away from this unelevated engine, so the save hotkey
// does nothing while such a game has focus. With the user's one-time consent
// a copy of this engine is placed in Program Files (admin-writable only) and a
// scheduled task runs it elevated as the relay: while an admin game is open the
// engine starts that task, and the relay reports presses of the save hotkey,
// and nothing else, over a named pipe.
class AdminHotkeyRelay {
public:
    using Trigger = std::function<void()>;

    explicit AdminHotkeyRelay(Trigger onTrigger);
    ~AdminHotkeyRelay();
    AdminHotkeyRelay(const AdminHotkeyRelay&) = delete;
    AdminHotkeyRelay& operator=(const AdminHotkeyRelay&) = delete;

    void setEnabled(bool enabled);
    void setAccelerator(const std::string& accelerator);
    // The foreground game's name when it runs as administrator, else empty.
    void observeForegroundGame(const std::string& elevatedGame);

    // The admin game seen most recently, kept for a minute after it closes.
    std::string elevatedGame() const;
    // That game has focus right now (the in-game notice shows only then).
    bool gameInFront() const;
    // off, not-installed, idle, starting, active, outdated, rejected or failed.
    std::string status() const;
    // missing, outdated or current, whether or not it is turned on.
    std::string setup() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Process entry points for `clipture_engine.exe --install-hotkey-relay <sid>`,
// `--uninstall-hotkey-relay <sid>` (both started elevated by the app) and
// `--hotkey-relay` (started by the scheduled task). Exit codes: 0 done,
// 2 bad arguments, 3 elevated as a different user than the one asking,
// 4 file copy failed, 5 task registration failed.
int installAdminHotkeyRelay(const std::wstring& forUserSid);
int uninstallAdminHotkeyRelay(const std::wstring& forUserSid);
int runAdminHotkeyRelay();

}  // namespace clipture::platform
