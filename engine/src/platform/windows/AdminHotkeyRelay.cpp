#include "clipture/platform/windows/AdminHotkeyRelay.hpp"

#include "clipture/AdminHotkeyRelayProtocol.hpp"
#include "clipture/MediaClock.hpp"
#include "clipture/RawInputHotkey.hpp"
#include "clipture/platform/ThreadName.hpp"
#include "clipture/platform/windows/ProcessElevation.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

namespace clipture::platform {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kTaskFolder[] = L"\\Clipture";
constexpr wchar_t kTaskPrefix[] = L"Hotkey relay ";

std::wstring relayFolder() {
    PWSTR programFiles = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles))) {
        folder = std::wstring(programFiles) + L"\\Clipture Hotkey Relay";
    }
    CoTaskMemFree(programFiles);
    return folder;
}

std::wstring relayExecutable() {
    const auto folder = relayFolder();
    return folder.empty() ? std::wstring() : folder + L"\\clipture_engine.exe";
}

std::wstring pipeName(const std::wstring& sid) { return L"\\\\.\\pipe\\clipture-hotkey-relay-" + sid; }
std::wstring taskName(const std::wstring& sid) { return kTaskPrefix + sid; }

bool sameFile(const std::wstring& left, const std::wstring& right) {
    return !left.empty() && CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring processPath(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};
    wchar_t path[MAX_PATH * 2] {};
    DWORD size = ARRAYSIZE(path);
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
    CloseHandle(process);
    return ok ? std::wstring(path, size) : std::wstring();
}

std::wstring xmlEscape(const std::wstring& text) {
    std::wstring result;
    for (const wchar_t c : text) {
        if (c == L'&') result += L"&amp;";
        else if (c == L'<') result += L"&lt;";
        else if (c == L'>') result += L"&gt;";
        else if (c == L'"') result += L"&quot;";
        else result += c;
    }
    return result;
}

// One pipe end with overlapped reads and writes, so the hotkey thread can write
// while another thread waits for the next line.
class PipeLink {
public:
    enum class Read { Line, Timeout, Interrupted, Closed };

    explicit PipeLink(HANDLE pipe)
        : pipe_(pipe), readEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          writeEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~PipeLink() {
        if (pending_) {
            CancelIoEx(pipe_, &readOverlapped_);
            DWORD ignored = 0;
            GetOverlappedResult(pipe_, &readOverlapped_, &ignored, TRUE);
        }
        CloseHandle(pipe_);
        if (readEvent_) CloseHandle(readEvent_);
        if (writeEvent_) CloseHandle(writeEvent_);
    }
    PipeLink(const PipeLink&) = delete;
    PipeLink& operator=(const PipeLink&) = delete;

    bool write(const std::string& line) {
        std::lock_guard lock(writeMutex_);
        OVERLAPPED overlapped {};
        overlapped.hEvent = writeEvent_;
        ResetEvent(writeEvent_);
        DWORD written = 0;
        if (!WriteFile(pipe_, line.data(), static_cast<DWORD>(line.size()), nullptr, &overlapped) &&
            GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        return GetOverlappedResult(pipe_, &overlapped, &written, TRUE) && written == line.size();
    }

    // The next complete line, without its newline.
    Read readLine(std::string& line, DWORD waitMs, HANDLE interrupt) {
        const ULONGLONG deadline = GetTickCount64() + waitMs;
        while (true) {
            if (const auto end = buffer_.find('\n'); end != std::string::npos) {
                line = buffer_.substr(0, end);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buffer_.erase(0, end + 1);
                return Read::Line;
            }
            if (buffer_.size() > 4096) return Read::Closed;  // no line is this long
            if (!pending_) {
                readOverlapped_ = {};
                readOverlapped_.hEvent = readEvent_;
                ResetEvent(readEvent_);
                if (!ReadFile(pipe_, chunk_, sizeof(chunk_), nullptr, &readOverlapped_) &&
                    GetLastError() != ERROR_IO_PENDING) {
                    return Read::Closed;
                }
                pending_ = true;
            }
            const ULONGLONG now = GetTickCount64();
            const DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
            HANDLE handles[] = {readEvent_, interrupt};
            const DWORD waited = WaitForMultipleObjects(interrupt ? 2 : 1, handles, FALSE, remaining);
            if (waited == WAIT_OBJECT_0 + 1) return Read::Interrupted;
            if (waited == WAIT_TIMEOUT) return Read::Timeout;
            if (waited != WAIT_OBJECT_0) return Read::Closed;
            DWORD received = 0;
            pending_ = false;
            if (!GetOverlappedResult(pipe_, &readOverlapped_, &received, FALSE) || received == 0) return Read::Closed;
            buffer_.append(chunk_, received);
        }
    }

private:
    HANDLE pipe_;
    HANDLE readEvent_;
    HANDLE writeEvent_;
    OVERLAPPED readOverlapped_ {};
    bool pending_ = false;
    char chunk_[512] {};
    std::string buffer_;
    std::mutex writeMutex_;
};

// Task Scheduler access on the calling thread (COM initialized by the caller).
ComPtr<ITaskService> taskService() {
    ComPtr<ITaskService> service;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)))) return nullptr;
    VARIANT empty {};
    if (FAILED(service->Connect(empty, empty, empty, empty))) return nullptr;
    return service;
}

struct Bstr {
    explicit Bstr(const std::wstring& text) : value(SysAllocString(text.c_str())) {}
    ~Bstr() { SysFreeString(value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    BSTR value;
};

ComPtr<IRegisteredTask> findTask(const std::wstring& sid) {
    auto service = taskService();
    ComPtr<ITaskFolder> folder;
    ComPtr<IRegisteredTask> task;
    Bstr folderPath(kTaskFolder);
    Bstr name(taskName(sid));
    if (!service || FAILED(service->GetFolder(folderPath.value, &folder)) ||
        FAILED(folder->GetTask(name.value, &task))) {
        return nullptr;
    }
    return task;
}

// The relay is a console program; Windows' headless console host runs it
// without ever showing a console window (1.8.1 ran it directly and showed one).
constexpr wchar_t kHeadlessMarker[] = L"--headless";

std::wstring consoleHost() {
    wchar_t system[MAX_PATH] {};
    const UINT length = GetSystemDirectoryW(system, ARRAYSIZE(system));
    return length ? std::wstring(system, length) + L"\\conhost.exe" : std::wstring();
}

enum class RelaySetup { Missing, Outdated, Current };

RelaySetup relaySetup(const std::wstring& sid) {
    const auto executable = relayExecutable();
    if (executable.empty() || GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) return RelaySetup::Missing;
    auto task = findTask(sid);
    if (!task) return RelaySetup::Missing;
    BSTR xml = nullptr;
    const bool headless = SUCCEEDED(task->get_Xml(&xml)) && xml && wcsstr(xml, kHeadlessMarker);
    SysFreeString(xml);
    return headless ? RelaySetup::Current : RelaySetup::Outdated;
}

bool runRelayTask(const std::wstring& sid) {
    auto task = findTask(sid);
    ComPtr<IRunningTask> running;
    VARIANT none {};
    return task && SUCCEEDED(task->Run(none, &running));
}

// Runs as the given user, elevated, only while that user is signed in; only
// administrators may change it, and the user may read and start it.
std::wstring taskXml(const std::wstring& sid, const std::wstring& executable) {
    return
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.3\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Author>Clipture</Author>"
        L"<Description>Lets Clipture's save hotkey work in games that run as administrator. "
        L"Clipture starts it only while such a game is open. Turn it off in Clipture's settings.</Description>"
        L"<SecurityDescriptor>D:(A;;FA;;;BA)(A;;FA;;;SY)(A;;GRGX;;;" + sid + L")</SecurityDescriptor>"
        L"</RegistrationInfo>"
        L"<Principals><Principal id=\"User\"><UserId>" + sid + L"</UserId>"
        L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><AllowStartOnDemand>true</AllowStartOnDemand>"
        L"<AllowHardTerminate>true</AllowHardTerminate><Priority>5</Priority><Enabled>true</Enabled>"
        L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
        L"</Settings>"
        L"<Actions Context=\"User\"><Exec><Command>" + xmlEscape(consoleHost()) + L"</Command>"
        L"<Arguments>" + std::wstring(kHeadlessMarker) + L" &quot;" + xmlEscape(executable) +
        L"&quot; --hotkey-relay</Arguments></Exec></Actions>"
        L"</Task>";
}

bool registerRelayTask(const std::wstring& sid, const std::wstring& executable) {
    auto service = taskService();
    if (!service) return false;
    ComPtr<ITaskFolder> root;
    ComPtr<ITaskFolder> folder;
    Bstr rootPath(L"\\");
    Bstr folderPath(kTaskFolder);
    if (FAILED(service->GetFolder(rootPath.value, &root))) return false;
    if (FAILED(service->GetFolder(folderPath.value, &folder))) {
        VARIANT sddl {};
        Bstr folderSddl(L"D:(A;;FA;;;BA)(A;;FA;;;SY)(A;;GRGX;;;AU)");
        sddl.vt = VT_BSTR;
        sddl.bstrVal = folderSddl.value;
        Bstr name(L"Clipture");
        if (FAILED(root->CreateFolder(name.value, sddl, &folder))) return false;
    }
    Bstr name(taskName(sid));
    Bstr xml(taskXml(sid, executable));
    VARIANT empty {};
    ComPtr<IRegisteredTask> registered;
    return SUCCEEDED(folder->RegisterTask(name.value, xml.value, TASK_CREATE_OR_UPDATE, empty, empty,
        TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered));
}

// Removes this user's task; the folder and the relay copy go with the last one.
bool removeRelayTask(const std::wstring& sid, bool& lastOne) {
    lastOne = false;
    auto service = taskService();
    ComPtr<ITaskFolder> folder;
    Bstr folderPath(kTaskFolder);
    if (!service || FAILED(service->GetFolder(folderPath.value, &folder))) {
        lastOne = true;
        return true;
    }
    Bstr name(taskName(sid));
    const HRESULT removed = folder->DeleteTask(name.value, 0);
    ComPtr<IRegisteredTaskCollection> tasks;
    LONG count = 0;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks))) tasks->get_Count(&count);
    if (count == 0) {
        lastOne = true;
        ComPtr<ITaskFolder> root;
        Bstr rootPath(L"\\");
        Bstr folderName(L"Clipture");
        if (SUCCEEDED(service->GetFolder(rootPath.value, &root))) root->DeleteFolder(folderName.value, 0);
    }
    return SUCCEEDED(removed) || removed == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

struct ComScope {
    ComScope() : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() { if (ok) CoUninitialize(); }
    bool ok;
};

}  // namespace

struct AdminHotkeyRelay::Impl {
    Trigger trigger;
    mutable std::mutex mutex;
    bool enabled = false;
    bool stopping = false;
    bool acceleratorChanged = false;
    std::string accelerator;
    std::string game;
    bool gameInFront = false;
    ULONGLONG gameSeenMs = 0;
    ULONGLONG retryAfterMs = 0;
    std::string state = "off";
    // missing, outdated or current: whether Allow must set it up again.
    std::string setup = "missing";
    ULONGLONG setupCheckedMs = 0;
    HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread worker;

    static constexpr ULONGLONG kGameMemoryMs = 60'000;
    static constexpr ULONGLONG kIdleStopMs = 120'000;

    void setState(const char* next) {
        std::lock_guard lock(mutex);
        state = next;
    }

    void run() {
        nameCurrentThread(L"admin-hotkey-relay");
        ComScope com;
        while (true) {
            refreshSetup();
            bool wanted = false;
            {
                std::lock_guard lock(mutex);
                if (stopping) return;
                const ULONGLONG now = GetTickCount64();
                if (!enabled) state = "off";
                wanted = enabled && !game.empty() && now >= retryAfterMs && !accelerator.empty();
            }
            if (!wanted) {
                WaitForSingleObject(signal, 1000);
                continue;
            }
            const auto sid = currentUserSid();
            const auto current = sid.empty() ? RelaySetup::Missing : relaySetup(sid);
            if (current != RelaySetup::Current) {
                // An outdated setup would show a console window: never start it.
                std::lock_guard lock(mutex);
                state = current == RelaySetup::Outdated ? "outdated" : "not-installed";
                retryAfterMs = GetTickCount64() + 10'000;
                continue;
            }
            setState("starting");
            session(sid);
        }
    }

    // Every 10 s, and at once after a change: Allow and removal happen in
    // another process, behind Windows' prompt.
    void refreshSetup(bool now = false) {
        const ULONGLONG tick = GetTickCount64();
        if (!now && setupCheckedMs && tick - setupCheckedMs < 10'000) return;
        setupCheckedMs = tick;
        const auto sid = currentUserSid();
        const auto current = sid.empty() ? RelaySetup::Missing : relaySetup(sid);
        std::lock_guard lock(mutex);
        setup = current == RelaySetup::Current ? "current" : current == RelaySetup::Outdated ? "outdated" : "missing";
    }

    void fail(const char* next, ULONGLONG retryMs) {
        std::lock_guard lock(mutex);
        state = next;
        retryAfterMs = retryMs == ULLONG_MAX ? ULLONG_MAX : GetTickCount64() + retryMs;
    }

    void session(const std::wstring& sid) {
        HANDLE pipe = CreateNamedPipeW(pipeName(sid).c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return fail("failed", 30'000);
        OVERLAPPED connect {};
        connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = ConnectNamedPipe(pipe, &connect) != 0 || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected && GetLastError() == ERROR_IO_PENDING) {
            if (!runRelayTask(sid)) {
                CancelIoEx(pipe, &connect);
            } else if (WaitForSingleObject(connect.hEvent, 10'000) == WAIT_OBJECT_0) {
                DWORD ignored = 0;
                connected = GetOverlappedResult(pipe, &connect, &ignored, FALSE) != 0;
            } else {
                CancelIoEx(pipe, &connect);
            }
        }
        if (!connected) {
            DWORD ignored = 0;
            GetOverlappedResult(pipe, &connect, &ignored, TRUE);
        }
        CloseHandle(connect.hEvent);
        if (!connected) {
            CloseHandle(pipe);
            return fail("failed", 30'000);
        }
        // Only the relay copy in Program Files may speak for the hotkey.
        ULONG client = 0;
        if (!GetNamedPipeClientProcessId(pipe, &client) || !sameFile(processPath(client), relayExecutable())) {
            CloseHandle(pipe);
            return fail("failed", 30'000);
        }
        PipeLink link(pipe);
        std::string accel;
        {
            std::lock_guard lock(mutex);
            accel = accelerator;
            acceleratorChanged = false;
        }
        std::string reply;
        if (!link.write(std::string(relay::kHello) + accel + "\n") ||
            link.readLine(reply, 5000, nullptr) != PipeLink::Read::Line) {
            return fail("failed", 30'000);
        }
        if (reply == "rejected") return fail("rejected", ULLONG_MAX);
        if (reply != "ok") return fail("outdated", ULLONG_MAX);
        setState("active");
        std::cerr << "[hotkey] Admin game hotkey relay is active.\n";
        while (true) {
            std::string line;
            const auto read = link.readLine(line, 5000, signal);
            if (read == PipeLink::Read::Closed) return fail("idle", 5000);
            if (read == PipeLink::Read::Line) {
                if (line == "hotkey" && trigger) trigger();
                continue;
            }
            std::unique_lock lock(mutex);
            const bool idle = game.empty() && GetTickCount64() - gameSeenMs > kIdleStopMs;
            if (stopping || !enabled || idle) {
                state = enabled ? "idle" : "off";
                lock.unlock();
                link.write("quit\n");
                return;
            }
            if (acceleratorChanged) {
                acceleratorChanged = false;
                const auto next = accelerator;
                lock.unlock();
                link.write(std::string(relay::kAcceleratorLine) + next + "\n");
            }
        }
    }
};

AdminHotkeyRelay::AdminHotkeyRelay(Trigger onTrigger) : impl_(std::make_unique<Impl>()) {
    impl_->trigger = std::move(onTrigger);
    impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
}

AdminHotkeyRelay::~AdminHotkeyRelay() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    SetEvent(impl_->signal);
    if (impl_->worker.joinable()) impl_->worker.join();
    CloseHandle(impl_->signal);
}

void AdminHotkeyRelay::setEnabled(bool enabled) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->enabled == enabled) return;
        impl_->enabled = enabled;
        // Allowing again (after an update or a refused shortcut) retries at once.
        impl_->retryAfterMs = 0;
        impl_->setupCheckedMs = 0;
    }
    SetEvent(impl_->signal);
}

void AdminHotkeyRelay::setAccelerator(const std::string& accelerator) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->accelerator == accelerator) return;
        impl_->accelerator = accelerator;
        impl_->acceleratorChanged = true;
        if (impl_->state == "rejected") impl_->retryAfterMs = 0;
    }
    SetEvent(impl_->signal);
}

void AdminHotkeyRelay::observeForegroundGame(const std::string& elevatedGame) {
    std::lock_guard lock(impl_->mutex);
    const ULONGLONG now = GetTickCount64();
    impl_->gameInFront = !elevatedGame.empty();
    if (!elevatedGame.empty()) {
        if (impl_->game != elevatedGame) SetEvent(impl_->signal);
        impl_->game = elevatedGame;
        impl_->gameSeenMs = now;
    } else if (!impl_->game.empty() && now - impl_->gameSeenMs > Impl::kGameMemoryMs) {
        impl_->game.clear();
    }
}

std::string AdminHotkeyRelay::elevatedGame() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->game;
}

bool AdminHotkeyRelay::gameInFront() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->gameInFront;
}

std::string AdminHotkeyRelay::setup() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->setup;
}

std::string AdminHotkeyRelay::status() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->state;
}

int installAdminHotkeyRelay(const std::wstring& forUserSid) {
    if (forUserSid.empty()) return 2;
    if (forUserSid != currentUserSid()) return 3;
    const auto folder = relayFolder();
    const auto target = relayExecutable();
    if (folder.empty()) return 4;
    CreateDirectoryW(folder.c_str(), nullptr);
    wchar_t self[MAX_PATH * 2] {};
    if (!GetModuleFileNameW(nullptr, self, ARRAYSIZE(self))) return 4;
    if (!sameFile(self, target)) {
        // A running relay keeps its file open; move it aside rather than fail.
        const auto previous = target + L".old";
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES &&
            !MoveFileExW(target.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            return 4;
        }
        if (!CopyFileW(self, target.c_str(), FALSE)) return 4;
        if (!DeleteFileW(previous.c_str())) MoveFileExW(previous.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    ComScope com;
    return registerRelayTask(forUserSid, target) ? 0 : 5;
}

int uninstallAdminHotkeyRelay(const std::wstring& forUserSid) {
    if (forUserSid.empty()) return 2;
    if (forUserSid != currentUserSid()) return 3;
    bool lastOne = false;
    {
        ComScope com;
        if (!removeRelayTask(forUserSid, lastOne)) return 5;
    }
    if (lastOne) {
        const auto target = relayExecutable();
        for (const auto& file : {target, target + L".old"}) {
            if (!DeleteFileW(file.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
                MoveFileExW(file.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
        }
        RemoveDirectoryW(relayFolder().c_str());
    }
    return 0;
}

int runAdminHotkeyRelay() {
    // Never a window, even when started without the headless console host.
    FreeConsole();
    const auto sid = currentUserSid();
    if (sid.empty()) return 2;
    const auto name = pipeName(sid);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 20 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
        WaitNamedPipeW(name.c_str(), 500);
        // Identification level: the engine may learn who the relay is, never act as it.
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    }
    if (pipe == INVALID_HANDLE_VALUE) return 2;
    PipeLink link(pipe);
    std::string hello;
    if (link.readLine(hello, 5000, nullptr) != PipeLink::Read::Line) return 2;
    const auto accelerator = relay::helloAccelerator(hello);
    if (!accelerator) {
        link.write("unsupported\n");
        return 2;
    }
    if (!relay::relaySafeAccelerator(*accelerator)) {
        link.write("rejected\n");
        return 2;
    }
    // Presses while an unelevated window has focus reach the engine directly.
    RawInputHotkey hotkey([&link] {
        const HWND foreground = GetForegroundWindow();
        DWORD processId = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &processId);
        const DWORD level = processId ? processIntegrityLevel(processId) : 0;
        if (processId && (level == 0 || level >= SECURITY_MANDATORY_HIGH_RID)) link.write("hotkey\n");
    });
    hotkey.configure(*accelerator);
    if (!link.write("ok\n")) return 2;
    std::optional<std::string> pending;
    int64_t lastChange100ns = monotonicNow100ns();
    while (true) {
        std::string line;
        const auto read = link.readLine(line, 1000, nullptr);
        if (read == PipeLink::Read::Closed) return 0;
        if (read == PipeLink::Read::Line) {
            if (line == "quit") return 0;
            if (line.rfind(relay::kAcceleratorLine, 0) == 0) pending = line.substr(relay::kAcceleratorLine.size());
        }
        const int64_t now = monotonicNow100ns();
        if (pending && now - lastChange100ns >= relay::kAcceleratorChangeInterval100ns) {
            if (relay::relaySafeAccelerator(*pending)) hotkey.configure(*pending);
            pending.reset();
            lastChange100ns = now;
        }
    }
}

}  // namespace clipture::platform
