#include "clipture/RawInputHotkey.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace clipture {
namespace {

constexpr std::uint32_t modifierControl = 1u << 0;
constexpr std::uint32_t modifierAlt = 1u << 1;
constexpr std::uint32_t modifierShift = 1u << 2;
constexpr std::uint32_t modifierSuper = 1u << 3;

struct ParsedAccelerator {
    std::uint32_t modifiers = 0;
    std::uint32_t virtualKey = 0;
};

std::string lowerTrimmed(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::uint32_t namedVirtualKey(const std::string& token) {
    if (token == "space") return VK_SPACE;
    if (token == "tab") return VK_TAB;
    if (token == "enter" || token == "return") return VK_RETURN;
    if (token == "escape" || token == "esc") return VK_ESCAPE;
    if (token == "backspace") return VK_BACK;
    if (token == "delete" || token == "del") return VK_DELETE;
    if (token == "insert" || token == "ins") return VK_INSERT;
    if (token == "home") return VK_HOME;
    if (token == "end") return VK_END;
    if (token == "pageup") return VK_PRIOR;
    if (token == "pagedown") return VK_NEXT;
    if (token == "up") return VK_UP;
    if (token == "down") return VK_DOWN;
    if (token == "left") return VK_LEFT;
    if (token == "right") return VK_RIGHT;
    if (token == "capslock") return VK_CAPITAL;
    if (token == "numlock") return VK_NUMLOCK;
    if (token == "scrolllock") return VK_SCROLL;
    if (token == "printscreen") return VK_SNAPSHOT;
    if (token == "pause") return VK_PAUSE;
    if (token == "plus") return VK_OEM_PLUS;
    if (token == "volumeup") return VK_VOLUME_UP;
    if (token == "volumedown") return VK_VOLUME_DOWN;
    if (token == "volumemute") return VK_VOLUME_MUTE;
    if (token == "medianexttrack") return VK_MEDIA_NEXT_TRACK;
    if (token == "mediaprevioustrack") return VK_MEDIA_PREV_TRACK;
    if (token == "mediastop") return VK_MEDIA_STOP;
    if (token == "mediaplaypause") return VK_MEDIA_PLAY_PAUSE;

    if (token.size() >= 2 && token[0] == 'f') {
        try {
            const int number = std::stoi(token.substr(1));
            if (number >= 1 && number <= 24) {
                return static_cast<std::uint32_t>(VK_F1 + number - 1);
            }
        } catch (...) {
            return 0;
        }
    }

    if (token.size() == 1) {
        const wchar_t character = static_cast<unsigned char>(token[0]);
        const SHORT mapped = VkKeyScanW(character);
        if (mapped != -1) return static_cast<std::uint32_t>(LOBYTE(mapped));
        const unsigned char ascii = static_cast<unsigned char>(token[0]);
        if (std::isalnum(ascii)) return static_cast<std::uint32_t>(std::toupper(ascii));
    }

    return 0;
}

ParsedAccelerator parseAccelerator(const std::string& accelerator) {
    ParsedAccelerator parsed;
    std::stringstream stream(accelerator);
    std::string part;
    while (std::getline(stream, part, '+')) {
        const auto token = lowerTrimmed(std::move(part));
        if (token.empty()) continue;
        if (token == "ctrl" || token == "control" || token == "cmdorctrl" || token == "commandorcontrol") {
            parsed.modifiers |= modifierControl;
        } else if (token == "alt" || token == "option") {
            parsed.modifiers |= modifierAlt;
        } else if (token == "shift") {
            parsed.modifiers |= modifierShift;
        } else if (token == "super" || token == "meta" || token == "command" || token == "cmd") {
            parsed.modifiers |= modifierSuper;
        } else {
            parsed.virtualKey = namedVirtualKey(token);
        }
    }
    return parsed;
}

UINT normalizedVirtualKey(const RAWKEYBOARD& keyboard) {
    UINT virtualKey = keyboard.VKey;
    if (virtualKey == 255) return 0;
    if (virtualKey == VK_SHIFT) {
        const UINT mapped = MapVirtualKeyW(keyboard.MakeCode, MAPVK_VSC_TO_VK_EX);
        return mapped != 0 ? mapped : VK_SHIFT;
    }
    if (virtualKey == VK_CONTROL) {
        return (keyboard.Flags & RI_KEY_E0) != 0 ? VK_RCONTROL : VK_LCONTROL;
    }
    if (virtualKey == VK_MENU) {
        return (keyboard.Flags & RI_KEY_E0) != 0 ? VK_RMENU : VK_LMENU;
    }
    return virtualKey;
}

}  // namespace

struct RawInputHotkey::Impl {
    explicit Impl(TriggerCallback callback)
        : onTrigger(std::move(callback)), worker([this] { run(); }) {
        std::unique_lock lock(startupMutex);
        startupCondition.wait_for(lock, std::chrono::seconds(2), [this] { return startupFinished; });
    }

    ~Impl() {
        const auto window = reinterpret_cast<HWND>(windowHandle.load(std::memory_order_acquire));
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        const DWORD id = threadId.load(std::memory_order_acquire);
        if (!window && id != 0) PostThreadMessageW(id, WM_QUIT, 0, 0);
        if (worker.joinable()) worker.join();
    }

    bool configure(const std::string& accelerator) {
        const auto parsed = parseAccelerator(accelerator);
        requiredModifiers.store(parsed.modifiers, std::memory_order_release);
        requiredVirtualKey.store(parsed.virtualKey, std::memory_order_release);

        std::lock_guard lock(statusMutex);
        if (accelerator.empty()) {
            statusText = rawInputReady.load(std::memory_order_acquire)
                ? "Raw Input ready; hotkey unassigned."
                : "Raw Input unavailable; hotkey unassigned.";
        } else if (parsed.virtualKey == 0) {
            statusText = "Raw Input ready, but the configured key is unsupported.";
        } else if (rawInputReady.load(std::memory_order_acquire)) {
            statusText = "Raw Input hotkey armed.";
        } else {
            statusText = "Raw Input hotkey failed to initialize.";
        }
        return parsed.virtualKey != 0 && rawInputReady.load(std::memory_order_acquire);
    }

    bool ready() const {
        return rawInputReady.load(std::memory_order_acquire);
    }

    std::string status() const {
        std::lock_guard lock(statusMutex);
        return statusText;
    }

    bool isDown(UINT virtualKey) const {
        return virtualKey < keyDown.size() && keyDown[virtualKey];
    }

    std::uint32_t activeModifiers() const {
        std::uint32_t modifiers = 0;
        if (isDown(VK_CONTROL) || isDown(VK_LCONTROL) || isDown(VK_RCONTROL)) modifiers |= modifierControl;
        if (isDown(VK_MENU) || isDown(VK_LMENU) || isDown(VK_RMENU)) modifiers |= modifierAlt;
        if (isDown(VK_SHIFT) || isDown(VK_LSHIFT) || isDown(VK_RSHIFT)) modifiers |= modifierShift;
        if (isDown(VK_LWIN) || isDown(VK_RWIN)) modifiers |= modifierSuper;
        return modifiers;
    }

    void handleKeyboard(const RAWKEYBOARD& keyboard) {
        const UINT virtualKey = normalizedVirtualKey(keyboard);
        if (virtualKey == 0 || virtualKey >= keyDown.size()) return;
        const bool released = (keyboard.Flags & RI_KEY_BREAK) != 0;
        const bool wasDown = keyDown[virtualKey];
        keyDown[virtualKey] = !released;
        if (released || wasDown) return;

        const auto target = requiredVirtualKey.load(std::memory_order_acquire);
        const auto modifiers = requiredModifiers.load(std::memory_order_acquire);
        if (target == 0 || virtualKey != target || activeModifiers() != modifiers) return;
        try {
            if (onTrigger) onTrigger();
        } catch (...) {
            // A hotkey callback must never terminate the input thread.
        }
    }

    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_INPUT) {
            RAWINPUT input {};
            UINT size = sizeof(input);
            const UINT read = GetRawInputData(
                reinterpret_cast<HRAWINPUT>(lParam),
                RID_INPUT,
                &input,
                &size,
                sizeof(RAWINPUTHEADER));
            if (read != static_cast<UINT>(-1) && input.header.dwType == RIM_TYPEKEYBOARD) {
                handleKeyboard(input.data.keyboard);
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }
        if (message == WM_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handleMessage(window, message, wParam, lParam)
                    : DefWindowProcW(window, message, wParam, lParam);
    }

    void finishStartup(bool ready, std::string status) {
        rawInputReady.store(ready, std::memory_order_release);
        {
            std::lock_guard statusLock(statusMutex);
            statusText = std::move(status);
        }
        {
            std::lock_guard startupLock(startupMutex);
            startupFinished = true;
        }
        startupCondition.notify_all();
    }

    void run() {
        threadId.store(GetCurrentThreadId(), std::memory_order_release);
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        constexpr wchar_t windowClassName[] = L"CliptureRawInputHotkeyWindow";
        WNDCLASSEXW windowClass {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Impl::windowProcedure;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = windowClassName;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            finishStartup(false, "Raw Input window class registration failed.");
            return;
        }

        HWND window = CreateWindowExW(
            0,
            windowClassName,
            L"",
            0,
            0,
            0,
            0,
            0,
            HWND_MESSAGE,
            nullptr,
            instance,
            this);
        if (!window) {
            finishStartup(false, "Raw Input message window creation failed.");
            return;
        }
        windowHandle.store(reinterpret_cast<std::uintptr_t>(window), std::memory_order_release);

        RAWINPUTDEVICE keyboard {};
        keyboard.usUsagePage = 0x01;
        keyboard.usUsage = 0x06;
        keyboard.dwFlags = RIDEV_INPUTSINK;
        keyboard.hwndTarget = window;
        if (!RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard))) {
            finishStartup(false, "Raw Input keyboard registration failed.");
            DestroyWindow(window);
            windowHandle.store(0, std::memory_order_release);
            return;
        }

        finishStartup(true, "Raw Input ready; hotkey unassigned.");
        MSG message {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        RAWINPUTDEVICE removeKeyboard {};
        removeKeyboard.usUsagePage = 0x01;
        removeKeyboard.usUsage = 0x06;
        removeKeyboard.dwFlags = RIDEV_REMOVE;
        removeKeyboard.hwndTarget = nullptr;
        RegisterRawInputDevices(&removeKeyboard, 1, sizeof(removeKeyboard));
        windowHandle.store(0, std::memory_order_release);
    }

    TriggerCallback onTrigger;
    std::atomic<DWORD> threadId {0};
    std::atomic<std::uintptr_t> windowHandle {0};
    std::atomic<bool> rawInputReady {false};
    std::atomic<std::uint32_t> requiredModifiers {0};
    std::atomic<std::uint32_t> requiredVirtualKey {0};
    std::array<bool, 256> keyDown {};
    mutable std::mutex statusMutex;
    std::string statusText = "Raw Input is starting.";
    std::mutex startupMutex;
    std::condition_variable startupCondition;
    bool startupFinished = false;
    std::thread worker;
};

RawInputHotkey::RawInputHotkey(TriggerCallback onTrigger)
    : impl_(std::make_unique<Impl>(std::move(onTrigger))) {}

RawInputHotkey::~RawInputHotkey() = default;

bool RawInputHotkey::configure(const std::string& accelerator) {
    return impl_->configure(accelerator);
}

bool RawInputHotkey::ready() const {
    return impl_->ready();
}

std::string RawInputHotkey::status() const {
    return impl_->status();
}

}  // namespace clipture
