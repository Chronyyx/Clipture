// Part of Engine.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
namespace {

// The whole-system loopback and the microphone feed only their own tracks
// (and the mixed track); with those off they are not captured at all.
bool coreSystemAudioNeeded(const clipture::EngineSettings& settings) {
    return settings.includeSystemAudio || settings.includeMixedAudio;
}
bool coreMicrophoneNeeded(const clipture::EngineSettings& settings) {
    return settings.includeMicrophoneAudio || settings.includeMixedAudio;
}

// Distinct from a crash in the host log: the GPU removed a device (driver reset).
constexpr UINT kDeviceRemovedExitCode = 75;

// How long an app keeps its track capture after its audio session goes quiet.
constexpr auto kAutoAppGrace = std::chrono::seconds(30);


std::mutex g_exeToGameNameMutex;
std::map<std::string, std::string> g_exeToGameName;

std::string getGameName(const std::string& exeName) {
    std::lock_guard<std::mutex> lock(g_exeToGameNameMutex);
    auto lower = exeName;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    auto it = g_exeToGameName.find(lower);
    if (it != g_exeToGameName.end()) return it->second;
    return exeName;
}

std::string nowIsoLike() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc {};
    gmtime_s(&utc, &time);
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string nowIdSuffix() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(milliseconds);
}

int64_t now100ns() {
    return monotonicNow100ns();
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << ch; break;
        }
    }
    return out.str();
}

using SaveTimingClock = std::chrono::steady_clock;
constexpr int64_t kHotReplayRetention100ns = 12LL * 10'000'000LL;
constexpr std::size_t kSaveWriteBurstBytes = 2u * 1024u * 1024u;
// Longest a save waits for frames still in the encoder (normally a few frame
// times); past it the last encoded frame is held to the save moment instead.
constexpr std::chrono::milliseconds kSaveEncoderCatchUpLimit{250};
constexpr uint64_t kMiB = 1024ULL * 1024ULL;

struct ReplayMemoryBudgets {
    std::size_t videoBytes = 0;
    std::size_t audioBytes = 0;
    std::size_t pcmRecoveryBytes = 0;
};

ReplaySegmentStoreOptions replayStoreOptions(const std::string& streamName, bool video) {
    ReplaySegmentStoreOptions options;
    options.streamName = streamName;
    options.targetSegmentDuration100ns = video ? 8LL * 10'000'000LL : 30LL * 10'000'000LL;
    options.targetSegmentBytes = video ? 64u * 1024u * 1024u : 8u * 1024u * 1024u;
    options.maximumWriteBytes = 512u * 1024u;
    options.alignSegmentsToKeyframes = video;
    const char* mp4Ready = std::getenv("CLIPTURE_REPLAY_MP4_READY");
    options.prepareMp4Samples = video && mp4Ready && std::string_view(mp4Ready) == "1";
    return options;
}

ReplayMemoryBudgets replayMemoryBudgets(const EngineSettings& settings) {
    uint64_t totalCacheCap = 160ULL * kMiB;
    MEMORYSTATUSEX memoryStatus {};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus)) {
        const uint64_t physicalCap = std::clamp<uint64_t>(
            memoryStatus.ullTotalPhys / 64ULL,
            96ULL * kMiB,
            192ULL * kMiB);
        const uint64_t availableCap = std::clamp<uint64_t>(
            memoryStatus.ullAvailPhys / 32ULL,
            64ULL * kMiB,
            physicalCap);
        totalCacheCap = std::min(physicalCap, availableCap);
    }

    const uint64_t retentionSeconds = static_cast<uint64_t>(
        std::max(1, settings.clipLengthSeconds + 5));
    const uint64_t encodedVideoBytes =
        retentionSeconds * static_cast<uint64_t>(std::max(1, settings.bitrateMbps)) * 1'000'000ULL / 8ULL;
    const uint64_t desiredVideoBytes =
        encodedVideoBytes + encodedVideoBytes * 15ULL / 100ULL + 16ULL * kMiB;

    std::size_t audioTrackCount = settings.appAudioProcesses.size();
    if (settings.includeSystemAudio) ++audioTrackCount;
    if (settings.includeMicrophoneAudio) ++audioTrackCount;
    if (settings.captureGameAudio) ++audioTrackCount;
    audioTrackCount = std::max<std::size_t>(audioTrackCount, 1);
    const uint64_t desiredAudioBytes = std::clamp<uint64_t>(
        retentionSeconds * static_cast<uint64_t>(audioTrackCount) * 24'000ULL + 4ULL * kMiB,
        8ULL * kMiB,
        16ULL * kMiB);
    const uint64_t videoCap = totalCacheCap > desiredAudioBytes
        ? totalCacheCap - desiredAudioBytes
        : totalCacheCap * 3ULL / 4ULL;

    return {
        static_cast<std::size_t>(std::min(desiredVideoBytes, videoCap)),
        static_cast<std::size_t>(desiredAudioBytes),
        static_cast<std::size_t>(desiredAudioBytes)
    };
}

int64_t saveTimingElapsedMs(SaveTimingClock::time_point startedAt) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SaveTimingClock::now() - startedAt).count();
}

void logEngineSaveTiming(const char* stage, SaveTimingClock::time_point startedAt, const std::string& details = {}) {
    std::cerr << "[save-timing] source=engine stage=" << stage
              << " ms=" << saveTimingElapsedMs(startedAt);
    if (!details.empty()) std::cerr << " " << details;
    std::cerr << std::endl;
}

std::string fixedNumber(double value, int precision = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string stripExtension(std::string value);
std::string lowerAscii(std::string value);

std::string displayAudioTrackName(const std::string& sourceId) {
    if (sourceId == "system-loopback-pcm") return "System audio";
    if (sourceId == "microphone-pcm") return "Microphone";
    if (sourceId == "mixed-preview-pcm") return "Mixed preview";
    if (sourceId.rfind("app:", 0) == 0) return stripExtension(sourceId.substr(4));
    if (sourceId.rfind("game:", 0) == 0) return stripExtension(sourceId.substr(5));
    return sourceId;
}

std::string stripExtension(std::string value) {
    const auto slash = value.find_last_of("\\/");
    if (slash != std::string::npos) value = value.substr(slash + 1);
    const auto dot = value.find_last_of('.');
    if (dot != std::string::npos) value = value.substr(0, dot);
    return value;
}

std::string stripKnownGameExecutableSuffix(std::string value) {
    const auto lower = lowerAscii(value);
    const std::vector<std::string> suffixes = {
        "-wingdk-shipping",
        "-win64-shipping",
        "-win32-shipping",
        "-windows-shipping",
        "-shipping"
    };
    for (const auto& suffix : suffixes) {
        if (lower.size() > suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0) {
            value.resize(value.size() - suffix.size());
            return value;
        }
    }
    return value;
}

bool isLikelyGameExecutableName(const std::string& exeName) {
    const auto lower = lowerAscii(std::filesystem::path(exeName).filename().string());
    return lower.size() > 4 && (
        lower.find("-wingdk-shipping.exe") != std::string::npos ||
        lower.find("-win64-shipping.exe") != std::string::npos ||
        lower.find("-win32-shipping.exe") != std::string::npos ||
        lower.find("-windows-shipping.exe") != std::string::npos
    );
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string narrowWide(const wchar_t* value) {
    if (!value) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string result(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), needed, nullptr, nullptr);
    return result;
}

std::string friendlyProcessName(const std::string& processName);

// The games Windows recognised, whatever launcher installed them. The
// detection loop rereads it when Windows signals a change.
platform::WindowsGameList& windowsGameList() {
    static platform::WindowsGameList list;
    return list;
}

// Steam and Epic manifests: they give a game its name, and catch games
// Windows has not listed (Game Bar off, never run yet).
InstalledGamesCatalog& installedGamesCatalog() {
    static InstalledGamesCatalog catalog;
    return catalog;
}

// The game the executable belongs to, or empty when it is no known game.
std::string detectInstalledGameByPath(const std::string& processPath) {
    auto name = installedGamesCatalog().match(processPath);
    if (name.empty() && windowsGameList().contains(processPath)) name = friendlyProcessName(processPath);
    return name;
}

std::string friendlyProcessName(const std::string& processName) {
    std::string lower = lowerAscii(std::filesystem::path(processName).filename().string());
    if (lower == "robloxplayerbeta.exe") return "Roblox";
    if (lower == "fortniteclient-win64-shipping.exe") return "Fortnite";
    if (lower == "valorant-win64-shipping.exe") return "VALORANT";
    if (lower == "cs2.exe") return "Counter-Strike 2";
    if (lower == "leagueclientuxrender.exe" || lower == "league of legends.exe") return "League of Legends";
    if (lower == "deadbydaylight-win64-shipping.exe") return "Dead by Daylight";
    if (lower == "minecraft.windows.exe" || lower == "javaw.exe") return "Minecraft";
    return stripKnownGameExecutableSuffix(stripExtension(processName));
}

std::string captureProcessName(const std::string& sourceSpec) {
    return audioProcessName(sourceSpec);
}

std::string sourceIdForProcessSpec(const std::string& sourceSpec) {
    return audioProcessSourceId(sourceSpec);
}

bool isPidProcessSpec(const std::string& sourceSpec) {
    return isPidAudioProcessSpec(sourceSpec);
}

DWORD processIdFromPidSourceSpec(const std::string& sourceSpec) {
    return static_cast<DWORD>(audioProcessId(sourceSpec));
}

bool containsExactSourceSpec(const std::vector<std::string>& sourceSpecs, const std::string& sourceSpec) {
    const auto wanted = lowerAscii(sourceSpec);
    return std::any_of(sourceSpecs.begin(), sourceSpecs.end(), [&](const std::string& candidate) {
        return lowerAscii(candidate) == wanted;
    });
}

std::string appPidSourceSpec(DWORD processId, const std::string& exeName) {
    return makePidAudioProcessSpec(AudioProcessKind::App, processId, exeName);
}

std::string gamePidSourceSpec(DWORD processId, const std::string& exeName) {
    return makePidAudioProcessSpec(AudioProcessKind::Game, processId, exeName);
}

bool containsProcessName(const std::vector<std::string>& sourceSpecs, const std::string& processName) {
    const auto wanted = lowerAscii(captureProcessName(processName));
    if (wanted.empty()) return true;
    return std::any_of(sourceSpecs.begin(), sourceSpecs.end(), [&](const std::string& sourceSpec) {
        return lowerAscii(captureProcessName(sourceSpec)) == wanted;
    });
}

std::vector<DWORD> activeAudioSessionProcessIds() {
    std::vector<DWORD> processIds;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return processIds;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        if (coInitialized) CoUninitialize();
        return processIds;
    }

    Microsoft::WRL::ComPtr<IMMDeviceCollection> devices;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(hr) || !devices) {
        if (coInitialized) CoUninitialize();
        return processIds;
    }

    UINT deviceCount = 0;
    devices->GetCount(&deviceCount);
    std::set<DWORD> seen;
    for (UINT deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex) {
        Microsoft::WRL::ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(deviceIndex, &device)) || !device) continue;

        Microsoft::WRL::ComPtr<IAudioSessionManager2> sessionManager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(sessionManager.GetAddressOf()))) || !sessionManager) {
            continue;
        }

        Microsoft::WRL::ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(sessionManager->GetSessionEnumerator(&sessions)) || !sessions) continue;

        int sessionCount = 0;
        sessions->GetCount(&sessionCount);
        for (int sessionIndex = 0; sessionIndex < sessionCount; ++sessionIndex) {
            Microsoft::WRL::ComPtr<IAudioSessionControl> session;
            if (FAILED(sessions->GetSession(sessionIndex, &session)) || !session) continue;

            AudioSessionState state = AudioSessionStateInactive;
            session->GetState(&state);
            if (state != AudioSessionStateActive) {
                Microsoft::WRL::ComPtr<IAudioMeterInformation> meter;
                float peak = 0.0f;
                if (FAILED(session.As(&meter)) || !meter || FAILED(meter->GetPeakValue(&peak)) || peak <= 0.0005f) {
                    continue;
                }
            }

            Microsoft::WRL::ComPtr<IAudioSessionControl2> session2;
            if (FAILED(session.As(&session2)) || !session2) continue;
            DWORD processId = 0;
            if (FAILED(session2->GetProcessId(&processId)) || processId == 0) continue;
            if (seen.insert(processId).second) processIds.push_back(processId);
        }
    }

    if (coInitialized) CoUninitialize();
    return processIds;
}

bool isIgnoredForegroundProcess(const std::string& exeName) {
    const auto lower = lowerAscii(exeName);
    return lower.empty() ||
        lower == "clipture.exe" ||
        lower == "clipture_engine.exe" ||
        lower == "electron.exe";
}

bool isRobloxProcess(const std::string& exeName) {
    return lowerAscii(std::filesystem::path(exeName).filename().string()) == "robloxplayerbeta.exe";
}

bool isMinimizedRobloxName(const std::string& name) {
    return lowerAscii(name) == "roblox minimized";
}

struct WindowSearchContext {
    DWORD processId = 0;
    bool foundVisibleRestoredWindow = false;
};

BOOL CALLBACK findVisibleRestoredWindowForProcess(HWND window, LPARAM lparam) {
    auto* context = reinterpret_cast<WindowSearchContext*>(lparam);
    DWORD windowProcessId = 0;
    GetWindowThreadProcessId(window, &windowProcessId);
    if (windowProcessId != context->processId) return TRUE;
    if (!IsWindowVisible(window) || IsIconic(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;

    wchar_t title[2] {};
    if (GetWindowTextW(window, title, 2) <= 0) return TRUE;

    context->foundVisibleRestoredWindow = true;
    return FALSE;
}

bool hasVisibleRestoredWindowForProcess(DWORD processId) {
    if (processId == 0) return false;
    WindowSearchContext context { processId, false };
    EnumWindows(findVisibleRestoredWindowForProcess, reinterpret_cast<LPARAM>(&context));
    return context.foundVisibleRestoredWindow;
}

std::string robloxWindowStateName(DWORD processId, HWND foregroundWindow = nullptr) {
    if (foregroundWindow && IsWindowVisible(foregroundWindow) && !IsIconic(foregroundWindow)) {
        return "Roblox";
    }
    return hasVisibleRestoredWindowForProcess(processId) ? "Roblox" : "Roblox minimized";
}

bool isKnownGameProcess(const std::string& exeName, const std::string& processPath, DWORD processId = 0, HWND foregroundWindow = nullptr) {
    if (isRobloxProcess(exeName)) {
        const auto robloxName = robloxWindowStateName(processId, foregroundWindow);
        std::lock_guard<std::mutex> lock(g_exeToGameNameMutex);
        g_exeToGameName[lowerAscii(exeName)] = robloxName;
        return !isMinimizedRobloxName(robloxName);
    }

    std::string gameName = detectInstalledGameByPath(processPath);
    if (!gameName.empty()) {
        std::lock_guard<std::mutex> lock(g_exeToGameNameMutex);
        g_exeToGameName[lowerAscii(exeName)] = gameName;
        return true;
    }
    gameName = friendlyProcessName(exeName);
    if (gameName != stripExtension(exeName) || isLikelyGameExecutableName(exeName)) {
        std::lock_guard<std::mutex> lock(g_exeToGameNameMutex);
        g_exeToGameName[lowerAscii(exeName)] = gameName;
        return true;
    }
    return false;
}

// The window in front and its process. A detection pass reads it once and
// every check in the pass uses that one reading.
struct ForegroundProcess {
    HWND window = nullptr;
    DWORD processId = 0;
    std::string path;
    std::string exeName;
};

ForegroundProcess readForegroundProcess() {
    ForegroundProcess foreground;
    foreground.window = GetForegroundWindow();
    if (!foreground.window) return foreground;
    GetWindowThreadProcessId(foreground.window, &foreground.processId);
    foreground.path = RunningProcessSnapshot::queryProcessPath(foreground.processId);
    const auto slash = foreground.path.find_last_of("\\/");
    foreground.exeName = slash == std::string::npos ? foreground.path : foreground.path.substr(slash + 1);
    return foreground;
}

std::pair<std::string, bool> detectForegroundAppInfo(const ForegroundProcess& foreground, HMONITOR activeMonitor) {
    const HWND window = foreground.window;
    if (!window) return {"Foreground app", false};

    if (activeMonitor) {
        HMONITOR windowMonitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        if (windowMonitor && windowMonitor != activeMonitor) {
            return {"Foreground app", false};
        }
    }

    const DWORD processId = foreground.processId;
    const std::string& processPath = foreground.path;

    if (!processPath.empty()) {
        const auto installedGame = detectInstalledGameByPath(processPath);
        if (!installedGame.empty()) return {installedGame, true};
        std::string exeName = std::filesystem::path(processPath).filename().string();
        if (isRobloxProcess(exeName)) return {robloxWindowStateName(processId, window), true};
        const auto friendly = friendlyProcessName(processPath);
        if (!friendly.empty() && friendly != "Clipture") {
            bool isGame = (friendly != stripExtension(exeName));
            return {friendly, isGame};
        }
    }

    wchar_t title[256] {};
    if (GetWindowTextW(window, title, 256) > 0) {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, title, -1, nullptr, 0, nullptr, nullptr);
        if (needed > 1) {
            std::string value(static_cast<std::size_t>(needed - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, title, -1, value.data(), needed, nullptr, nullptr);
            return {value, false};
        }
    }
    return {"Foreground app", false};
}

struct ForegroundGameCaptureTarget {
    HWND window = nullptr;
    std::string name;
};

bool experimentalGameWindowCaptureEnabled() {
    static const bool enabled = [] {
        char value[16] {};
        const DWORD length = GetEnvironmentVariableA(
            "CLIPTURE_EXPERIMENTAL_GAME_WINDOW_CAPTURE",
            value,
            static_cast<DWORD>(sizeof(value)));
        if (length == 0 || length >= sizeof(value)) return false;
        const auto flag = lowerAscii(std::string(value, length));
        return flag == "1" || flag == "true" || flag == "on";
    }();
    return enabled;
}

// `isKnownGame`: the foreground process is a known game (see the detection loop).
ForegroundGameCaptureTarget detectForegroundGameCaptureTarget(
    const ForegroundProcess& foreground,
    bool isKnownGame,
    HMONITOR activeMonitor) {
    const HWND window = foreground.window;
    if (!window || !IsWindowVisible(window) || IsIconic(window)) return {};

    if (activeMonitor) {
        const HMONITOR windowMonitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        if (windowMonitor && windowMonitor != activeMonitor) return {};
    }
    if (!isKnownGame) return {};
    return { window, getGameName(foreground.exeName) };
}

// GPU priority needs the game even when no audio option runs the detection
// pass (ADR 0029): the known game in front, and the name of every running
// process. Only a game that once starved recording matches the per-game memory,
// and a raised game only needs to be found by name.
// Every running process name, once each.
void runningProcessNames(const RunningProcessSnapshot& processes, std::vector<std::string>& names) {
    std::unordered_set<std::string> seen;
    for (const auto& entry : processes.entries()) {
        if (seen.insert(entry.lowerExeName).second) names.push_back(entry.exeName);
    }
}

void detectGamesForGpuPriority(
    const ForegroundProcess& foreground,
    bool foregroundIsKnownGame,
    const RunningProcessSnapshot& processes,
    std::string& foregroundGameExe,
    std::vector<std::string>& runningExes) {
    if (foregroundGameExe.empty() && foregroundIsKnownGame) foregroundGameExe = foreground.exeName;
    runningProcessNames(processes, runningExes);
}

// The app in front when no known game is, for GPU priority (ADR 0029,
// 2026-11-20): an unlisted or elevated game is raised and remembered by its
// executable name. Clipture itself and the Windows shell are no app.
std::string gpuPriorityApp(const ForegroundProcess& foreground) {
    if (isIgnoredForegroundProcess(foreground.exeName)) return {};
    const auto lower = lowerAscii(foreground.exeName);
    for (const char* shell : { "explorer.exe", "shellexperiencehost.exe", "startmenuexperiencehost.exe",
                               "searchhost.exe", "lockapp.exe", "applicationframehost.exe" }) {
        if (lower == shell) return {};
    }
    return foreground.exeName;
}

// Lower-case process names of a list of audio source specs, for testing many
// process names against the same list.
class ProcessNameSet {
public:
    explicit ProcessNameSet(const std::vector<std::string>& sourceSpecs) {
        for (const auto& sourceSpec : sourceSpecs) names_.insert(lowerAscii(captureProcessName(sourceSpec)));
    }

    // As containsProcessName(): a process without a name counts as listed.
    bool contains(const std::string& lowerExeName) const {
        return lowerExeName.empty() || names_.contains(lowerExeName);
    }

private:
    std::unordered_set<std::string> names_;
};

// Whether a process is a game, remembered while it runs (ProcessVerdictCache).
struct GameVerdict {
    enum class Kind { NotGame, Game, Unreadable };
    Kind kind = Kind::NotGame;
    // Unreadable: when to ask for the path again.
    std::chrono::steady_clock::time_point retryAt {};
};

// A process whose path could not be read is asked again this often: its
// protection can change after it starts.
constexpr auto kUnreadablePathRetry = std::chrono::seconds(30);
std::pair<int, int> parseResolution(const std::string& resolution) {
    const auto marker = resolution.find('x');
    if (marker == std::string::npos) return { 1920, 1080 };
    return {
        std::max(1, std::stoi(resolution.substr(0, marker))),
        std::max(1, std::stoi(resolution.substr(marker + 1)))
    };
}

std::string formatResolution(int width, int height) {
    std::ostringstream resolution;
    resolution << width << "x" << height;
    return resolution.str();
}

std::pair<int, int> encodedResolutionFromPackets(const std::vector<EncodedPacket>& packets) {
    int bestWidth = 0;
    int bestHeight = 0;
    for (const auto& packet : packets) {
        if (packet.encodedWidth <= 0 || packet.encodedHeight <= 0) continue;
        if (bestWidth <= 0 || bestHeight <= 0 ||
            packet.encodedWidth * packet.encodedHeight > bestWidth * bestHeight) {
            bestWidth = packet.encodedWidth;
            bestHeight = packet.encodedHeight;
        }
    }
    return { bestWidth, bestHeight };
}

std::pair<int, int> lowestSourceResolutionFromPackets(const std::vector<EncodedPacket>& packets) {
    int bestWidth = 0;
    int bestHeight = 0;
    for (const auto& packet : packets) {
        if (packet.sourceWidth <= 0 || packet.sourceHeight <= 0) continue;
        if (bestWidth <= 0 || bestHeight <= 0 ||
            packet.sourceWidth * packet.sourceHeight < bestWidth * bestHeight) {
            bestWidth = packet.sourceWidth;
            bestHeight = packet.sourceHeight;
        }
    }
    return { bestWidth, bestHeight };
}

struct VideoSegmentRange {
    std::size_t begin = 0;
    std::size_t end = 0;
    int width = 0;
    int height = 0;
};

std::vector<VideoSegmentRange> splitVideoSegmentRangesByEncodedResolution(const std::vector<EncodedPacket>& packets) {
    std::vector<VideoSegmentRange> segments;
    int currentWidth = 0;
    int currentHeight = 0;
    for (std::size_t index = 0; index < packets.size(); ++index) {
        const auto& packet = packets[index];
        if (packet.kind != PacketKind::Video || payloadEmpty(packet)) continue;
        const int packetWidth = packet.encodedWidth > 0 ? packet.encodedWidth : currentWidth;
        const int packetHeight = packet.encodedHeight > 0 ? packet.encodedHeight : currentHeight;
        if (segments.empty() ||
            (packetWidth > 0 && packetHeight > 0 && (packetWidth != currentWidth || packetHeight != currentHeight))) {
            if (!segments.empty()) segments.back().end = index;
            currentWidth = packetWidth;
            currentHeight = packetHeight;
            segments.push_back({ index, packets.size(), currentWidth, currentHeight });
        }
    }
    if (!segments.empty()) segments.back().end = packets.size();
    return segments;
}

std::string stitchedPathForSegments(const std::string& firstSegmentPath) {
    const auto dot = firstSegmentPath.find_last_of('.');
    if (dot == std::string::npos) return firstSegmentPath + ".stitched.mp4";
    return firstSegmentPath.substr(0, dot) + ".stitched.mp4";
}

std::pair<int, int> encoderMaxDimensions(const EngineSettings& settings) {
    if (settings.targetWidth > 0 && settings.targetHeight > 0) {
        return { settings.targetWidth, settings.targetHeight };
    }
    return maxDisplayDimensions();
}

int actualClipDurationSeconds(const std::vector<EncodedPacket>& packets, int requestedDurationSeconds) {
    if (packets.empty()) return requestedDurationSeconds;
    const int64_t firstPts = packets.front().pts100ns;
    const int64_t lastPts = packets.back().pts100ns;
    const int64_t lastDuration = packets.back().duration100ns > 0 ? packets.back().duration100ns : 0;
    const int64_t span = std::max<int64_t>(0, lastPts - firstPts + lastDuration);
    return std::clamp(static_cast<int>((span + 5'000'000LL) / 10'000'000LL), 1, requestedDurationSeconds);
}

std::vector<EncodedPacket> selectVideoWindowForClip(
    std::vector<EncodedPacket> video,
    int durationSeconds,
    int64_t saveHorizon100ns) {
    std::erase_if(video, [](const EncodedPacket& packet) {
        return packet.kind != PacketKind::Video || payloadEmpty(packet);
    });
    if (video.empty()) return {};

    const int64_t newestPts = video.back().pts100ns;
    const int64_t newestDuration = std::max<int64_t>(1, video.back().duration100ns);
    const int64_t selectedEnd = std::max(saveHorizon100ns, newestPts + newestDuration);
    const int64_t requestedStart = std::max<int64_t>(
        0,
        selectedEnd - static_cast<int64_t>(durationSeconds) * 10'000'000LL);

    std::size_t firstIndex = video.size();
    for (std::size_t i = 0; i < video.size(); ++i) {
        if (video[i].pts100ns >= requestedStart) {
            firstIndex = i;
            break;
        }
    }
    if (firstIndex == video.size()) firstIndex = video.size() - 1;

    for (std::size_t i = firstIndex + 1; i > 0; --i) {
        const std::size_t index = i - 1;
        if (video[index].keyframe) {
            firstIndex = index;
            break;
        }
    }

    if (!video[firstIndex].keyframe) {
        for (std::size_t i = firstIndex; i < video.size(); ++i) {
            if (video[i].keyframe) {
                firstIndex = i;
                break;
            }
        }
    }

    if (!video[firstIndex].keyframe) firstIndex = 0;

    video.erase(video.begin(), video.begin() + static_cast<std::ptrdiff_t>(firstIndex));
    video.back().duration100ns = std::max<int64_t>(
        newestDuration,
        selectedEnd - video.back().pts100ns);
    return video;
}

}  // namespace

