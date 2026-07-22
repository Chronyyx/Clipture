#pragma once

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace clipture {

struct RunningProcessInfo {
    DWORD processId = 0;
    DWORD parentProcessId = 0;
    std::string exeName;
    std::string lowerExeName;
};

class RunningProcessSnapshot {
public:
    static RunningProcessSnapshot captureNameOnly() {
        RunningProcessSnapshot snapshot;
        HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (processes == INVALID_HANDLE_VALUE) return snapshot;

        PROCESSENTRY32W entry {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(processes, &entry)) {
            do {
                auto exeName = narrow(entry.szExeFile);
                snapshot.entries_.push_back({
                    entry.th32ProcessID,
                    entry.th32ParentProcessID,
                    exeName,
                    lowerAscii(exeName)
                });
            } while (Process32NextW(processes, &entry));
        }
        CloseHandle(processes);
        snapshot.valid_ = true;
        return snapshot;
    }

    static std::string queryProcessPath(DWORD processId) {
        if (processId == 0) return {};
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process) return {};

        std::wstring path(32768, L'\0');
        DWORD size = static_cast<DWORD>(path.size());
        std::string result;
        if (QueryFullProcessImageNameW(process, 0, path.data(), &size) && size > 0) {
            path.resize(size);
            result = narrow(path.c_str());
        }
        CloseHandle(process);
        return result;
    }

    bool contains(std::string processName) const {
        return processIdForName(std::move(processName)) != 0;
    }

    DWORD processIdForName(std::string processName) const {
        const auto wanted = lowerAscii(captureProcessName(std::move(processName)));
        if (wanted.empty()) return 0;
        const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const RunningProcessInfo& entry) {
            return entry.lowerExeName == wanted;
        });
        return found == entries_.end() ? 0 : found->processId;
    }

    const std::vector<RunningProcessInfo>& entries() const {
        return entries_;
    }

    std::size_t size() const {
        return entries_.size();
    }

    bool valid() const {
        return valid_;
    }

private:
    static std::string captureProcessName(std::string sourceSpec) {
        if (sourceSpec.rfind("app-pid:", 0) == 0) {
            const auto nameStart = sourceSpec.find(':', 8);
            return nameStart == std::string::npos ? std::string{} : sourceSpec.substr(nameStart + 1);
        }
        if (sourceSpec.rfind("game:", 0) == 0) return sourceSpec.substr(5);
        if (sourceSpec.rfind("app:", 0) == 0) return sourceSpec.substr(4);
        return sourceSpec;
    }

    static std::string lowerAscii(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    static std::string narrow(const wchar_t* value) {
        if (!value) return {};
        const int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) return {};
        std::string result(static_cast<std::size_t>(needed - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), needed, nullptr, nullptr);
        return result;
    }

    std::vector<RunningProcessInfo> entries_;
    bool valid_ = false;
};

}  // namespace clipture
