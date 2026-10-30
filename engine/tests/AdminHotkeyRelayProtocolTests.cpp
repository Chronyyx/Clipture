#include "clipture/AdminHotkeyRelayProtocol.hpp"
#include <cstdlib>
#include <iostream>
static void require(bool value) { if (!value) std::abort(); }
int main() {
    using namespace clipture::relay;
    // Shortcuts the elevated relay may watch: a modifier other than Shift, or a
    // key that types nothing.
    for (const char* safe : {"Ctrl+Shift+S", "ctrl+s", "Alt+F10", "Shift+F9", "F8", "f24", "Ctrl + Plus",
             "CommandOrControl+Shift+R", "Insert", "Shift+Pause", "Super+G", "PageDown", "MediaPlayPause"}) {
        require(relaySafeAccelerator(safe));
    }
    // Typing: letters, digits, Shift+letter, Space and Enter would let a shortcut
    // reveal what someone types in an admin window.
    for (const char* unsafe : {"", "S", "Shift+S", "1", "Space", "Enter", "Shift+Space", "F25", "F0", "F",
             "Ctrl", "Ctrl+Shift", "Ctrl+A+B", "Ctrl++", "Tab", "Backspace",
             "Ctrl+aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
        require(!relaySafeAccelerator(unsafe));
    }
    require(helloAccelerator("clipture-hotkey-relay/1 Ctrl+Shift+S") == std::optional<std::string>("Ctrl+Shift+S"));
    require(!helloAccelerator("clipture-hotkey-relay/2 Ctrl+Shift+S"));
    require(!helloAccelerator("hotkey"));
    std::cout << "Admin hotkey relay accepts shortcuts only and checks its protocol version.\n";
}
