#pragma once
#include <Windows.h>

namespace clipture {
// Names the calling thread for debuggers and per-thread CPU accounting.
inline void nameCurrentThread(const wchar_t* name) {
    SetThreadDescription(GetCurrentThread(), name);
}
}  // namespace clipture
