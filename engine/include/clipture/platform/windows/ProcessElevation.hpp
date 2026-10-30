#pragma once

#include <Windows.h>

#include <string>

namespace clipture::platform {

// Mandatory integrity level of a process (SECURITY_MANDATORY_*_RID), or 0
// when it cannot be read.
DWORD processIntegrityLevel(DWORD processId);

// Whether a process runs above this one, as a game started as administrator
// does. Windows then blocks this process's input hooks and raw input while that
// game has focus. A process this one may not even query (anti-cheat protection)
// counts as above.
bool runsAboveCurrentProcess(DWORD processId);

// The current user's SID as a string (S-1-5-...), empty on failure.
std::wstring currentUserSid();

}  // namespace clipture::platform
