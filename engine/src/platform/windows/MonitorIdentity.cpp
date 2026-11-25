#include "clipture/platform/windows/MonitorIdentity.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace clipture::platform {
namespace {
constexpr char kPrefix[] = "monitor:";

std::string lowerUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string result(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), needed, nullptr, nullptr);
    for (auto& character : result) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return result;
}
}  // namespace

std::string stableMonitorIdFromDevicePath(const std::wstring& devicePath) {
    const auto path = lowerUtf8(devicePath);
    return path.empty() ? std::string {} : kPrefix + path;
}

bool isStableMonitorId(const std::string& id) {
    return id.rfind(kPrefix, 0) == 0 && id.size() > sizeof(kPrefix) - 1;
}

std::string stableMonitorId(const wchar_t* gdiName) {
    if (!gdiName || !*gdiName) return {};
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return {};
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return {};
    }
    for (UINT32 index = 0; index < pathCount; ++index) {
        const auto& path = paths[index];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            wcscmp(gdiName, source.viewGdiDeviceName) != 0) {
            continue;
        }
        DISPLAYCONFIG_TARGET_DEVICE_NAME target {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) return {};
        return stableMonitorIdFromDevicePath(target.monitorDevicePath);
    }
    return {};
}

}  // namespace clipture::platform
