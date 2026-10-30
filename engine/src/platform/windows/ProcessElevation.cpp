#include "clipture/platform/windows/ProcessElevation.hpp"

#include <sddl.h>

#include <vector>

namespace clipture::platform {
namespace {

DWORD tokenIntegrityLevel(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    if (!size) return 0;
    std::vector<unsigned char> buffer(size);
    if (!GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) return 0;
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
    return count ? *GetSidSubAuthority(label->Label.Sid, count - 1) : 0;
}

}  // namespace

DWORD processIntegrityLevel(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return 0;
    HANDLE token = nullptr;
    DWORD level = 0;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        level = tokenIntegrityLevel(token);
        CloseHandle(token);
    }
    CloseHandle(process);
    return level;
}

bool runsAboveCurrentProcess(DWORD processId) {
    if (!processId) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    const DWORD own = tokenIntegrityLevel(token);
    CloseHandle(token);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
    CloseHandle(process);
    const DWORD other = processIntegrityLevel(processId);
    // A token that cannot be read belongs to a process walled off from this one.
    return other == 0 || other > own;
}

std::wstring currentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size ? size : 1);
    std::wstring result;
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
            result = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return result;
}

}  // namespace clipture::platform
