#pragma once

#include "clipture/AudioProcessSpec.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace clipture {

namespace detail {

// Head of one SystemProcessInformation record (SYSTEM_PROCESS_INFORMATION in
// winternl.h, with its reserved fields named); thread records follow it.
struct SystemProcessRecord {
    ULONG nextEntryOffset;
    ULONG numberOfThreads;
    BYTE reserved[48];
    USHORT imageNameBytes;
    USHORT imageNameCapacity;
    PWSTR imageName;
    LONG basePriority;
    HANDLE processId;
    HANDLE parentProcessId;
};
#ifdef _WIN64
static_assert(offsetof(SystemProcessRecord, imageName) == 64);
static_assert(offsetof(SystemProcessRecord, processId) == 80);
static_assert(offsetof(SystemProcessRecord, parentProcessId) == 88);
#endif

}  // namespace detail

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
        if (captureFromSystemList(snapshot)) return snapshot;
        return captureFromToolhelp();
    }

    // The same list through Toolhelp, which maps its snapshot section again
    // for every entry: about twice the time. Used when the system call fails.
    static RunningProcessSnapshot captureFromToolhelp() {
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

        std::string result;
        wchar_t shortPath[MAX_PATH] {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, shortPath, &size)) {
            if (size > 0) result = narrow(shortPath, static_cast<int>(size));
        } else if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            std::wstring longPath(32768, L'\0');
            size = static_cast<DWORD>(longPath.size());
            if (QueryFullProcessImageNameW(process, 0, longPath.data(), &size) && size > 0) {
                result = narrow(longPath.data(), static_cast<int>(size));
            }
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

    // The whole list in one system call, in the order Toolhelp reports it.
    // False when the call is unavailable; `snapshot` is then left empty.
    static bool captureFromSystemList(RunningProcessSnapshot& snapshot) {
        using QuerySystemInformation = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
        static const auto query = [] {
            const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            return ntdll
                ? reinterpret_cast<QuerySystemInformation>(GetProcAddress(ntdll, "NtQuerySystemInformation"))
                : nullptr;
        }();
        if (!query) return false;

        constexpr ULONG kSystemProcessInformation = 5;
        constexpr LONG kLengthMismatch = static_cast<LONG>(0xC0000004);
        constexpr ULONG kHeadroom = 128 * 1024;
        static std::atomic<ULONG> lastSize { 1024 * 1024 };
        ULONG size = lastSize.load(std::memory_order_relaxed);
        std::unique_ptr<unsigned char[]> buffer;
        LONG status = kLengthMismatch;
        for (int attempt = 0; attempt < 4 && status == kLengthMismatch; ++attempt) {
            buffer.reset(new (std::nothrow) unsigned char[size]);
            if (!buffer) return false;
            ULONG needed = 0;
            status = query(kSystemProcessInformation, buffer.get(), size, &needed);
            if (status == kLengthMismatch) size = std::max(needed, size) + kHeadroom;
        }
        if (status != 0) return false;
        lastSize.store(size, std::memory_order_relaxed);

        std::size_t offset = 0;
        for (;;) {
            if (offset + sizeof(detail::SystemProcessRecord) > size) {
                snapshot.entries_.clear();
                return false;
            }
            const auto* record = reinterpret_cast<const detail::SystemProcessRecord*>(buffer.get() + offset);
            const auto processId = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(record->processId));
            // Toolhelp's name for the idle process, which has none of its own.
            auto exeName = processId == 0
                ? std::string("[System Process]")
                : narrow(record->imageName, static_cast<int>(record->imageNameBytes / sizeof(wchar_t)));
            auto lowerExeName = lowerAscii(exeName);
            snapshot.entries_.push_back({
                processId,
                static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(record->parentProcessId)),
                std::move(exeName),
                std::move(lowerExeName)
            });
            if (record->nextEntryOffset == 0) break;
            offset += record->nextEntryOffset;
        }
        snapshot.valid_ = true;
        return true;
    }

private:
    static std::string captureProcessName(std::string sourceSpec) {
        return audioProcessName(sourceSpec);
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

    static std::string narrow(const wchar_t* value, int length) {
        if (!value || length <= 0) return {};
        const int needed = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string result(static_cast<std::size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, length, result.data(), needed, nullptr, nullptr);
        return result;
    }

    std::vector<RunningProcessInfo> entries_;
    bool valid_ = false;
};

inline const RunningProcessInfo* findProcessInfo(
    const std::vector<RunningProcessInfo>& entries,
    DWORD processId) {
    if (processId == 0) return nullptr;
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const RunningProcessInfo& entry) {
        return entry.processId == processId;
    });
    return found == entries.end() ? nullptr : &*found;
}

inline const RunningProcessInfo* findProcessInfo(
    const RunningProcessSnapshot& snapshot,
    DWORD processId) {
    return findProcessInfo(snapshot.entries(), processId);
}

inline bool processHasAncestor(
    const std::vector<RunningProcessInfo>& entries,
    DWORD childProcessId,
    DWORD ancestorProcessId) {
    if (childProcessId == 0 || ancestorProcessId == 0 || childProcessId == ancestorProcessId) return false;
    DWORD current = childProcessId;
    for (int depth = 0; depth < 24; ++depth) {
        const auto* info = findProcessInfo(entries, current);
        if (!info || info->parentProcessId == 0 || info->parentProcessId == current) return false;
        if (info->parentProcessId == ancestorProcessId) return true;
        current = info->parentProcessId;
    }
    return false;
}

inline bool processHasAncestor(
    const RunningProcessSnapshot& snapshot,
    DWORD childProcessId,
    DWORD ancestorProcessId) {
    return processHasAncestor(snapshot.entries(), childProcessId, ancestorProcessId);
}

inline std::vector<DWORD> collapseProcessTreeRoots(
    const std::vector<RunningProcessInfo>& entries,
    const std::vector<DWORD>& candidateProcessIds) {
    std::vector<DWORD> roots;
    for (const DWORD processId : candidateProcessIds) {
        if (processId == 0 || std::find(roots.begin(), roots.end(), processId) != roots.end()) continue;
        const bool alreadyCovered = std::any_of(roots.begin(), roots.end(), [&](DWORD rootProcessId) {
            return processHasAncestor(entries, processId, rootProcessId);
        });
        if (alreadyCovered) continue;

        std::erase_if(roots, [&](DWORD rootProcessId) {
            return processHasAncestor(entries, rootProcessId, processId);
        });
        roots.push_back(processId);
    }
    return roots;
}

// The app captures for the sounding processes (process ID -> executable name):
// one per process tree, every root kept. Roots with the same executable name
// share its track, since their source id carries no PID. Discord plays voice
// from its renderer and join sounds from its audio service, sibling processes
// under a silent main process, so keeping only one root per name lost voices.
inline std::vector<std::string> appAudioCaptureSpecs(
    const std::vector<RunningProcessInfo>& entries,
    const std::map<DWORD, std::string>& soundingExeById) {
    std::vector<DWORD> processIds;
    processIds.reserve(soundingExeById.size());
    for (const auto& [processId, _] : soundingExeById) processIds.push_back(processId);
    std::vector<std::string> specs;
    for (const DWORD root : collapseProcessTreeRoots(entries, processIds)) {
        auto spec = makePidAudioProcessSpec(AudioProcessKind::App, root, soundingExeById.at(root));
        if (!spec.empty()) specs.push_back(std::move(spec));
    }
    return specs;
}

inline DWORD preferredProcessTreeRootForName(
    const std::vector<RunningProcessInfo>& entries,
    std::string processName) {
    processName = audioProcessName(processName);
    std::transform(processName.begin(), processName.end(), processName.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (processName.empty()) return 0;

    std::vector<DWORD> candidates;
    for (const auto& entry : entries) {
        std::string lowerExeName = entry.lowerExeName;
        if (lowerExeName.empty()) {
            lowerExeName = entry.exeName;
            std::transform(lowerExeName.begin(), lowerExeName.end(), lowerExeName.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
        }
        if (lowerExeName == processName) candidates.push_back(entry.processId);
    }
    if (candidates.empty()) return 0;

    const auto roots = collapseProcessTreeRoots(entries, candidates);
    DWORD preferredRoot = 0;
    std::size_t preferredCoverage = 0;
    for (const DWORD root : roots) {
        const auto coverage = static_cast<std::size_t>(std::count_if(
            candidates.begin(),
            candidates.end(),
            [&](DWORD candidate) {
                return candidate == root || processHasAncestor(entries, candidate, root);
            }));
        if (coverage > preferredCoverage ||
            (coverage == preferredCoverage && (preferredRoot == 0 || root < preferredRoot))) {
            preferredRoot = root;
            preferredCoverage = coverage;
        }
    }
    return preferredRoot;
}

}  // namespace clipture
