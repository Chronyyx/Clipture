#include "clipture/platform/windows/WindowsGameList.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace clipture::platform {
namespace {

// ASCII only, as process paths are compared elsewhere in the engine.
std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), needed, nullptr, nullptr);
    return result;
}

std::wstring readString(HKEY key, const wchar_t* subkey, const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(key, subkey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes <= sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, subkey, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

// A TitleId means Windows matched the entry to its game database.
bool hasTitleId(HKEY key, const wchar_t* subkey) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegGetValueW(key, subkey, L"TitleId", RRF_RT_ANY, &type, nullptr, &bytes) != ERROR_SUCCESS) return false;
    return type == REG_SZ ? bytes > sizeof(wchar_t) : bytes > 0;
}

}  // namespace

WindowsGameList::WindowsGameList(std::wstring childrenKey)
    : childrenKey_(std::move(childrenKey)), paths_(std::make_shared<const std::unordered_set<std::string>>()) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, childrenKey_.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    key_ = key;
    changed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // Armed before the first read, so a change during it is not missed.
    watch();
    read();
}

WindowsGameList::~WindowsGameList() {
    if (key_) RegCloseKey(static_cast<HKEY>(key_));
    if (changed_) CloseHandle(changed_);
}

void WindowsGameList::watch() {
    if (!key_ || !changed_) return;
    const DWORD filter = REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC;
    if (RegNotifyChangeKeyValue(static_cast<HKEY>(key_), TRUE, filter, changed_, TRUE) != ERROR_SUCCESS) {
        CloseHandle(changed_);
        changed_ = nullptr;  // the list stays as first read
    }
}

void WindowsGameList::read() {
    auto paths = std::make_shared<std::unordered_set<std::string>>();
    const auto key = static_cast<HKEY>(key_);
    wchar_t name[256];
    for (DWORD index = 0;; ++index) {
        DWORD length = static_cast<DWORD>(std::size(name));
        const auto status = RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;
        if (!hasTitleId(key, name)) continue;
        const auto path = readString(key, name, L"MatchedExeFullPath");
        if (!path.empty()) paths->insert(lowerAscii(utf8(path)));
    }
    std::lock_guard lock(mutex_);
    paths_ = std::move(paths);
}

bool WindowsGameList::refreshIfChanged() {
    if (!changed_ || WaitForSingleObject(changed_, 0) != WAIT_OBJECT_0) return false;
    watch();
    read();
    return true;
}

bool WindowsGameList::contains(const std::string& processPath) const {
    if (processPath.empty()) return false;
    std::shared_ptr<const std::unordered_set<std::string>> paths;
    {
        std::lock_guard lock(mutex_);
        paths = paths_;
    }
    return paths->contains(lowerAscii(processPath));
}

std::size_t WindowsGameList::size() const {
    std::lock_guard lock(mutex_);
    return paths_->size();
}

}  // namespace clipture::platform
