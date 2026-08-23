#include "clipture/Engine.hpp"
#include "clipture/AudioProcessSpec.hpp"
#include "clipture/CaptureSession.hpp"
#include "clipture/MediaClock.hpp"
#include "clipture/Mp4Muxer.hpp"
#include "clipture/NativeRuntime.hpp"
#include "clipture/ProcessSnapshot.hpp"
#include "clipture/VideoCadenceAnalysis.hpp"

#include <Windows.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <limits>
#include <sstream>
#include <regex>
#include <cstdlib>
#include <unordered_set>
#include <vector>
#include <thread>
#include <atomic>

#include <map>
#include <mutex>
#include <set>
#include <span>

namespace clipture {
namespace {

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

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

std::string extractQuotedField(const std::string& text, const std::string& field) {
    const std::regex pattern("\"" + field + "\"\\s+\"([^\"]+)\"");
    std::smatch match;
    if (std::regex_search(text, match, pattern) && match.size() > 1) return match[1].str();
    const std::regex jsonPattern("\"" + field + "\"\\s*:\\s*\"([^\"]+)\"");
    if (std::regex_search(text, match, jsonPattern) && match.size() > 1) return match[1].str();
    return {};
}

struct GameInstall {
    std::string name;
    std::filesystem::path root;
};

bool pathStartsWith(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const auto childText = lowerAscii(child.lexically_normal().string());
    auto parentText = lowerAscii(parent.lexically_normal().string());
    if (!parentText.empty() && parentText.back() != '/' && parentText.back() != '\\') parentText.push_back('\\');
    return childText.rfind(parentText, 0) == 0;
}

std::vector<std::filesystem::path> steamLibraries() {
    std::vector<std::filesystem::path> libraries;
    const char* programFilesX86 = std::getenv("ProgramFiles(x86)");
    std::filesystem::path steamRoot = programFilesX86 ? std::filesystem::path(programFilesX86) / "Steam" : "C:\\Program Files (x86)\\Steam";
    libraries.push_back(steamRoot);

    const auto libraryFile = steamRoot / "steamapps" / "libraryfolders.vdf";
    const auto text = readTextFile(libraryFile);
    const std::regex pathPattern("\"path\"\\s+\"([^\"]+)\"");
    for (std::sregex_iterator it(text.begin(), text.end(), pathPattern), end; it != end; ++it) {
        auto value = (*it)[1].str();
        std::replace(value.begin(), value.end(), '\\', '/');
        libraries.emplace_back(value);
    }
    return libraries;
}

std::vector<GameInstall> scanInstalledGames() {
    static std::vector<GameInstall> cachedGames;
    static bool hasScanned = false;
    if (hasScanned) return cachedGames;
    hasScanned = true;

    for (const auto& library : steamLibraries()) {
        const auto steamApps = library / "steamapps";
        std::error_code ignored;
        if (!std::filesystem::exists(steamApps, ignored)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(steamApps, ignored)) {
            if (!entry.is_regular_file(ignored) || entry.path().filename().string().rfind("appmanifest_", 0) != 0) continue;
            const auto text = readTextFile(entry.path());
            const auto name = extractQuotedField(text, "name");
            const auto installDir = extractQuotedField(text, "installdir");
            if (!name.empty() && !installDir.empty()) cachedGames.push_back({ name, steamApps / "common" / installDir });
        }
    }

    const char* programData = std::getenv("ProgramData");
    const std::filesystem::path epicManifests = (programData ? std::filesystem::path(programData) : "C:\\ProgramData") /
        "Epic" / "EpicGamesLauncher" / "Data" / "Manifests";
    std::error_code ignored;
    if (std::filesystem::exists(epicManifests, ignored)) {
        for (const auto& entry : std::filesystem::directory_iterator(epicManifests, ignored)) {
            if (!entry.is_regular_file(ignored) || entry.path().extension() != ".item") continue;
            const auto text = readTextFile(entry.path());
            const auto name = extractQuotedField(text, "DisplayName");
            const auto installLocation = extractQuotedField(text, "InstallLocation");
            if (!name.empty() && !installLocation.empty()) cachedGames.push_back({ name, installLocation });
        }
    }
    return cachedGames;
}

std::string detectInstalledGameByPath(const std::string& processPath) {
    if (processPath.empty()) return {};
    const std::filesystem::path path(processPath);
    const auto games = scanInstalledGames();
    std::size_t bestLength = 0;
    std::string bestName;
    for (const auto& game : games) {
        if (game.root.empty() || !pathStartsWith(path, game.root)) continue;
        const auto length = game.root.string().size();
        if (length > bestLength) {
            bestLength = length;
            bestName = game.name;
        }
    }
    return bestName;
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

std::pair<std::string, bool> detectForegroundAppInfo(HMONITOR activeMonitor = nullptr) {
    HWND window = GetForegroundWindow();
    if (!window) return {"Foreground app", false};

    if (activeMonitor) {
        HMONITOR windowMonitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        if (windowMonitor && windowMonitor != activeMonitor) {
            return {"Foreground app", false};
        }
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    std::string processPath;
    if (processId != 0) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (process) {
            wchar_t path[MAX_PATH] {};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &size) && size > 0) {
                processPath = narrowWide(path);
            }
            CloseHandle(process);
        }
    }

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

ForegroundGameCaptureTarget detectForegroundGameCaptureTarget(HMONITOR activeMonitor) {
    HWND window = GetForegroundWindow();
    if (!window || !IsWindowVisible(window) || IsIconic(window)) return {};

    if (activeMonitor) {
        const HMONITOR windowMonitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        if (windowMonitor && windowMonitor != activeMonitor) return {};
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0) return {};

    const auto processPath = RunningProcessSnapshot::queryProcessPath(processId);
    if (processPath.empty()) return {};

    const auto exeName = std::filesystem::path(processPath).filename().string();
    if (isIgnoredForegroundProcess(exeName) ||
        !isKnownGameProcess(exeName, processPath, processId, window)) {
        return {};
    }
    return { window, getGameName(exeName) };
}
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

Engine::Engine()
    : diagnostics_(collectDiagnostics()),
      frameQueue_(20),
      videoPackets_(5LL * 60LL * 10'000'000LL, 16u * 1024u * 1024u),
      audioPackets_(5LL * 60LL * 10'000'000LL, 4u * 1024u * 1024u),
      aacAudioPackets_(5LL * 60LL * 10'000'000LL, 4u * 1024u * 1024u),
      videoReplayStore_(std::make_unique<ReplaySegmentStore>(replayStoreOptions("video", true))),
      aacReplayStore_(std::make_unique<ReplaySegmentStore>(replayStoreOptions("audio", false))),
      pcmRecoveryStore_(std::make_unique<ReplaySegmentStore>(replayStoreOptions("pcm-recovery", false))),
      audioReplayCoordinator_(std::make_unique<AudioReplayCoordinator>(
          audioPackets_,
          aacAudioPackets_,
          aacReplayStore_.get(),
          pcmRecoveryStore_.get())),
      captureSession_(std::make_unique<CaptureSession>()),
      encoderWorker_(std::make_unique<EncoderWorker>(
          frameQueue_,
          videoPackets_,
          videoReplayStore_.get())),
      audioCaptureWorker_(std::make_unique<AudioCaptureWorker>(audioPackets_, audioReplayCoordinator_.get())) {}

Engine::~Engine() {
    gameDetectionRunning_ = false;
    gameDetectionWaitCv_.notify_all();
    if (gameDetectionThread_.joinable()) {
        gameDetectionThread_.join();
    }
    if (audioCaptureWorker_) audioCaptureWorker_->stop();
    if (audioReplayCoordinator_) audioReplayCoordinator_->stop();
    if (captureSession_) captureSession_->stop();
    if (encoderWorker_) encoderWorker_->stop();
    if (pcmRecoveryStore_) pcmRecoveryStore_->stop();
    if (aacReplayStore_) aacReplayStore_->stop();
    if (videoReplayStore_) videoReplayStore_->stop();
}

void Engine::refreshAudioRouting() {
    if (!audioReplayCoordinator_) return;
    std::map<std::string, std::string> routes;
    if (settings_.includeSystemAudio) routes["system-loopback-pcm"] = "system-loopback-pcm";
    if (settings_.includeMicrophoneAudio) routes["microphone-pcm"] = "microphone-pcm";

    std::vector<std::string> activeSources;
    std::map<std::string, std::string> aliases;
    {
        std::lock_guard<std::mutex> lock(appAudioSourcesMutex_);
        activeSources = activeAppAudioSources_;
        aliases = activeAppAudioTrackAliases_;
    }
    if (activeSources.empty()) {
        activeSources = settings_.appAudioProcesses;
        for (const auto& systemProcess : settings_.systemAudioProcesses) {
            if (!containsProcessName(activeSources, systemProcess)) activeSources.push_back(systemProcess);
            aliases[sourceIdForProcessSpec(systemProcess)] = "system-loopback-pcm";
        }
    }
    for (const auto& sourceSpec : activeSources) {
        const auto sourceId = sourceIdForProcessSpec(sourceSpec);
        const auto alias = aliases.find(sourceId);
        if (alias != aliases.end()) {
            routes[sourceId] = alias->second;
        } else if (sourceId.rfind("game:", 0) == 0 && settings_.captureGameAudio) {
            routes[sourceId] = "game:" + getGameName(sourceId.substr(5));
        } else if (sourceId.rfind("app:", 0) == 0) {
            routes[sourceId] = sourceId;
        }
    }
    audioReplayCoordinator_->updateRouting(std::move(routes));
}

void Engine::gameDetectionLoop() {
    {
        std::unique_lock lock(gameDetectionWaitMutex_);
        if (gameDetectionWaitCv_.wait_for(lock, std::chrono::milliseconds(900), [this] {
                return !gameDetectionRunning_.load();
            })) {
            return;
        }
    }
    if (audioCaptureWorker_) audioCaptureWorker_->startConfiguredAppSources();

    std::size_t detectionPasses = 0;
    while (gameDetectionRunning_) {
        {
            HMONITOR activeMonitor = captureSession_ ? static_cast<HMONITOR>(captureSession_->activeMonitor()) : nullptr;
            auto [fgApp, fgIsGame] = detectForegroundAppInfo(activeMonitor);
            if (experimentalGameWindowCaptureEnabled()) {
                const auto gameCaptureTarget = detectForegroundGameCaptureTarget(activeMonitor);
                if (captureSession_ && gameCaptureTarget.window) {
                    captureSession_->preferGameWindow(gameCaptureTarget.window, gameCaptureTarget.name);
                    activeGameCaptureWindow_ = gameCaptureTarget.window;
                } else if (captureSession_ && activeGameCaptureWindow_ &&
                           (!IsWindow(static_cast<HWND>(activeGameCaptureWindow_)) ||
                            IsIconic(static_cast<HWND>(activeGameCaptureWindow_)))) {
                    captureSession_->preferMonitor();
                    activeGameCaptureWindow_ = nullptr;
                }
            } else if (captureSession_ && activeGameCaptureWindow_) {
                captureSession_->preferMonitor();
                activeGameCaptureWindow_ = nullptr;
            }

            std::lock_guard<std::mutex> lock(historyMutex_);
            auto now = std::chrono::steady_clock::now();
            if (fgApp != "Foreground app") {
                foregroundHistory_.push_back({now, fgApp, fgIsGame});
            }
            const auto cutoff = now - std::chrono::minutes(10);
            while (!foregroundHistory_.empty() && foregroundHistory_.front().timestamp < cutoff) {
                foregroundHistory_.pop_front();
            }
        }

        if ((settings_.captureGameAudio || settings_.captureForegroundSystemAudio) && audioCaptureWorker_) {
            const auto detectionStartedAt = std::chrono::steady_clock::now();
            const auto processSnapshot = RunningProcessSnapshot::captureNameOnly();
            std::size_t pathQueries = 0;
            std::vector<std::string> dynamicSources;
            std::vector<std::string> detectedGames;
            std::vector<std::string> foregroundSystemApps;
            std::map<std::string, std::string> dynamicTrackAliases;
            struct GameCaptureCandidate {
                DWORD processId = 0;
                std::string exeName;
                std::string gameName;
            };
            std::vector<GameCaptureCandidate> gameCandidates;
            auto sourceProcessMatches = [](const std::string& sourceSpec, const std::string& processName) {
                const auto sourceName = lowerAscii(captureProcessName(sourceSpec));
                const auto wantedName = lowerAscii(captureProcessName(processName));
                return !sourceName.empty() && !wantedName.empty() && sourceName == wantedName;
            };
            auto forgetForegroundSystemSource = [&](const std::string& processName) {
                foregroundSystemProcesses_.erase(
                    std::remove_if(
                        foregroundSystemProcesses_.begin(),
                        foregroundSystemProcesses_.end(),
                        [&](const std::string& sourceSpec) {
                            return sourceProcessMatches(sourceSpec, processName);
                        }),
                    foregroundSystemProcesses_.end());
            };
            auto rememberForegroundSystemSource = [&](const std::string& sourceSpec) {
                if (sourceSpec.empty()) return;
                foregroundSystemProcesses_.erase(
                    std::remove_if(
                        foregroundSystemProcesses_.begin(),
                        foregroundSystemProcesses_.end(),
                        [&](const std::string& name) {
                            if (isPidProcessSpec(sourceSpec)) {
                                if (!isPidProcessSpec(name) && sourceProcessMatches(name, sourceSpec)) return true;
                                return lowerAscii(name) == lowerAscii(sourceSpec);
                            }
                            if (isPidProcessSpec(name)) {
                                return lowerAscii(name) == lowerAscii(sourceSpec);
                            }
                            return lowerAscii(captureProcessName(name)) == lowerAscii(captureProcessName(sourceSpec));
                        }),
                    foregroundSystemProcesses_.end());
                foregroundSystemProcesses_.insert(foregroundSystemProcesses_.begin(), sourceSpec);
                if (foregroundSystemProcesses_.size() > 6) {
                    foregroundSystemProcesses_.resize(6);
                }
            };
            auto rememberGameCandidate = [&](DWORD processId, const std::string& exeName) {
                if (processId == 0 || exeName.empty()) return;
                if (std::any_of(gameCandidates.begin(), gameCandidates.end(), [&](const GameCaptureCandidate& candidate) {
                        return candidate.processId == processId;
                    })) {
                    return;
                }
                forgetForegroundSystemSource(exeName);
                gameCandidates.push_back({ processId, exeName, getGameName(exeName) });
            };
            
            HWND foreground = GetForegroundWindow();
            if (foreground) {
                DWORD fgPid = 0;
                GetWindowThreadProcessId(foreground, &fgPid);
                if (fgPid > 0) {
                    ++pathQueries;
                    const auto narrowPath = RunningProcessSnapshot::queryProcessPath(fgPid);
                    if (!narrowPath.empty()) {
                        auto slashPos = narrowPath.find_last_of("\\/");
                        std::string fgExeName = (slashPos != std::string::npos) ? narrowPath.substr(slashPos + 1) : narrowPath;

                        if (!isIgnoredForegroundProcess(fgExeName)) {
                            const bool isGame = isKnownGameProcess(fgExeName, narrowPath, fgPid, foreground);
                            if (settings_.captureGameAudio && isGame &&
                                !containsProcessName(settings_.appAudioProcesses, fgExeName) &&
                                !containsProcessName(settings_.systemAudioProcesses, fgExeName) &&
                                !std::any_of(gameCandidates.begin(), gameCandidates.end(), [&](const GameCaptureCandidate& candidate) {
                                    return candidate.processId == fgPid;
                                })) {
                                rememberGameCandidate(fgPid, fgExeName);
                            } else if ((settings_.captureForegroundSystemAudio || settings_.captureGameAudio) && !isGame &&
                                !containsProcessName(settings_.appAudioProcesses, fgExeName) &&
                                !containsProcessName(settings_.systemAudioProcesses, fgExeName)) {
                                const auto foregroundSpec = appPidSourceSpec(fgPid, fgExeName);
                                const auto foregroundSource = foregroundSpec.empty() ? fgExeName : foregroundSpec;
                                rememberForegroundSystemSource(foregroundSource);
                                dynamicTrackAliases[sourceIdForProcessSpec(foregroundSource)] = "system-loopback-pcm";
                                foregroundSystemApps.push_back(fgExeName);

                                for (const auto audioProcessId : activeAudioSessionProcessIds()) {
                                    if (audioProcessId == fgPid) continue;
                                    const auto* audioProcess = findProcessInfo(processSnapshot, audioProcessId);
                                    if (!audioProcess || audioProcess->exeName.empty()) continue;
                                    if (isIgnoredForegroundProcess(audioProcess->exeName) ||
                                        containsProcessName(settings_.appAudioProcesses, audioProcess->exeName) ||
                                        containsProcessName(settings_.systemAudioProcesses, audioProcess->exeName)) {
                                        continue;
                                    }

                                    const bool relatedByTree =
                                        processHasAncestor(processSnapshot, audioProcessId, fgPid) ||
                                        processHasAncestor(processSnapshot, fgPid, audioProcessId);
                                    const bool alreadyCoveredByForegroundTree =
                                        processHasAncestor(processSnapshot, audioProcessId, fgPid);
                                    const bool relatedBySameExe = lowerAscii(audioProcess->exeName) == lowerAscii(fgExeName);
                                    const bool alreadyCoveredByName = containsProcessName(dynamicSources, audioProcess->exeName);
                                    if (alreadyCoveredByForegroundTree) continue;
                                    if (alreadyCoveredByName && (!relatedBySameExe || relatedByTree)) continue;
                                    const bool relatedToForeground = relatedByTree || relatedBySameExe;
                                    if (!relatedToForeground) continue;

                                    const auto helperSpec = appPidSourceSpec(audioProcessId, audioProcess->exeName);
                                    if (helperSpec.empty()) continue;
                                    rememberForegroundSystemSource(helperSpec);
                                    dynamicTrackAliases[sourceIdForProcessSpec(helperSpec)] = "system-loopback-pcm";
                                    foregroundSystemApps.push_back(audioProcess->exeName);
                                }
                            }
                        }
                    }
                }
            }
            
            if (settings_.captureGameAudio) {
                static const std::unordered_set<std::string> kSystemExes = {
                    "svchost.exe", "conhost.exe", "csrss.exe", "services.exe", "lsass.exe",
                    "dwm.exe", "smss.exe", "taskhostw.exe", "runtimebroker.exe", "searchhost.exe",
                    "audiodg.exe", "sihost.exe", "fontdrvhost.exe", "dllhost.exe", "ctfmon.exe",
                    "shellexperiencehost.exe", "startmenuexperiencehost.exe", "textinputhost.exe",
                    "systemsettings.exe", "securityhealthservice.exe", "smartscreen.exe",
                    "explorer.exe", "searchindexer.exe", "wudfhost.exe"
                };

                for (const auto& entry : processSnapshot.entries()) {
                    const auto& exeName = entry.exeName;
                    if (kSystemExes.contains(lowerAscii(exeName)) ||
                        containsProcessName(settings_.appAudioProcesses, exeName) ||
                        containsProcessName(settings_.systemAudioProcesses, exeName) ||
                        std::any_of(gameCandidates.begin(), gameCandidates.end(), [&](const GameCaptureCandidate& candidate) {
                            return candidate.processId == entry.processId;
                        })) {
                        continue;
                    }

                    bool isGame = false;
                    ++pathQueries;
                    const auto processPath = RunningProcessSnapshot::queryProcessPath(entry.processId);
                    if (!processPath.empty()) {
                        isGame = isKnownGameProcess(exeName, processPath, entry.processId);
                    }

                    if (isGame) {
                        rememberGameCandidate(entry.processId, exeName);
                    }
                }

                std::map<std::string, std::vector<const GameCaptureCandidate*>> candidatesByGame;
                for (const auto& candidate : gameCandidates) {
                    candidatesByGame[lowerAscii(candidate.gameName)].push_back(&candidate);
                }
                for (const auto& [_, candidates] : candidatesByGame) {
                    std::vector<DWORD> candidateProcessIds;
                    candidateProcessIds.reserve(candidates.size());
                    for (const auto* candidate : candidates) candidateProcessIds.push_back(candidate->processId);

                    const auto roots = collapseProcessTreeRoots(processSnapshot.entries(), candidateProcessIds);
                    for (const DWORD rootProcessId : roots) {
                        const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const GameCaptureCandidate* candidate) {
                            return candidate->processId == rootProcessId;
                        });
                        if (found == candidates.end()) continue;

                        const auto& candidate = **found;
                        const auto sourceSpec = gamePidSourceSpec(candidate.processId, candidate.exeName);
                        if (sourceSpec.empty()) continue;
                        dynamicSources.push_back(sourceSpec);
                        dynamicTrackAliases[sourceIdForProcessSpec(sourceSpec)] = "game:" + candidate.gameName;
                        if (std::find(detectedGames.begin(), detectedGames.end(), candidate.gameName) == detectedGames.end()) {
                            detectedGames.push_back(candidate.gameName);
                        }
                    }
                }
            }

            if (settings_.captureForegroundSystemAudio || settings_.captureGameAudio) {
                foregroundSystemProcesses_.erase(
                    std::remove_if(
                        foregroundSystemProcesses_.begin(),
                        foregroundSystemProcesses_.end(),
                        [&](const std::string& name) {
                            const DWORD foregroundProcessId = isPidProcessSpec(name)
                                ? processIdFromPidSourceSpec(name)
                                : processSnapshot.processIdForName(name);
                            const bool processStillRunning = foregroundProcessId != 0 &&
                                findProcessInfo(processSnapshot, foregroundProcessId) != nullptr;
                            const bool nowCapturedAsGame = settings_.captureGameAudio &&
                                std::any_of(
                                    dynamicSources.begin(),
                                    dynamicSources.end(),
                                    [&](const std::string& sourceSpec) {
                                        const auto parsed = parseAudioProcessSpec(sourceSpec);
                                        if (parsed.kind != AudioProcessKind::Game) return false;
                                        if (sourceProcessMatches(sourceSpec, name)) return true;
                                        const DWORD gameProcessId = static_cast<DWORD>(parsed.processId);
                                        return foregroundProcessId != 0 && gameProcessId != 0 &&
                                            (processHasAncestor(processSnapshot, foregroundProcessId, gameProcessId) ||
                                             processHasAncestor(processSnapshot, gameProcessId, foregroundProcessId));
                                    });
                            return isIgnoredForegroundProcess(captureProcessName(name)) ||
                                containsProcessName(settings_.appAudioProcesses, name) ||
                                containsProcessName(settings_.systemAudioProcesses, name) ||
                                nowCapturedAsGame ||
                                !processStillRunning;
                        }),
                    foregroundSystemProcesses_.end());

                if (foregroundSystemProcesses_.size() > 6) {
                    foregroundSystemProcesses_.resize(6);
                }

                for (const auto& processName : foregroundSystemProcesses_) {
                    const bool alreadyAdded = isPidProcessSpec(processName)
                        ? containsExactSourceSpec(dynamicSources, processName)
                        : containsProcessName(dynamicSources, processName);
                    if (!alreadyAdded) {
                        dynamicSources.push_back(processName);
                        dynamicTrackAliases[sourceIdForProcessSpec(processName)] = "system-loopback-pcm";
                    }
                }
            }
            
            std::vector<std::string> mergedSources = settings_.appAudioProcesses;
            for (const auto& systemProcess : settings_.systemAudioProcesses) {
                if (!containsProcessName(mergedSources, systemProcess)) mergedSources.push_back(systemProcess);
                dynamicTrackAliases[sourceIdForProcessSpec(systemProcess)] = "system-loopback-pcm";
            }
            for (const auto& sourceSpec : dynamicSources) {
                const bool alreadyMerged = isPidProcessSpec(sourceSpec)
                    ? containsExactSourceSpec(mergedSources, sourceSpec)
                    : containsProcessName(mergedSources, sourceSpec);
                if (!alreadyMerged) {
                    mergedSources.push_back(sourceSpec);
                }
            }
            bool audioSourcesChanged = false;
            {
                std::lock_guard<std::mutex> lock(appAudioSourcesMutex_);
                audioSourcesChanged =
                    activeAppAudioSources_ != mergedSources ||
                    activeAppAudioTrackAliases_ != dynamicTrackAliases;
                activeAppAudioSources_ = mergedSources;
                activeAppAudioTrackAliases_ = dynamicTrackAliases;
            }
            if (audioSourcesChanged && !detectedGames.empty()) {
                std::cerr << "[engine] Detected games in loop:";
                for (const auto& game : detectedGames) std::cerr << " " << game;
                std::cerr << std::endl;
            }
            if (audioSourcesChanged && !foregroundSystemApps.empty()) {
                std::cerr << "[engine] Foreground app added to system-specific audio:";
                for (const auto& app : foregroundSystemApps) std::cerr << " " << app;
                std::cerr << std::endl;
            }
            audioCaptureWorker_->configureAppSources(mergedSources);
            refreshAudioRouting();

            ++detectionPasses;
            const auto detectionMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - detectionStartedAt).count();
            if (detectionMs >= 50 || detectionPasses % 30 == 0) {
                std::cerr << "[perf] game_detection"
                          << " ms=" << detectionMs
                          << " processes=" << processSnapshot.size()
                          << " pathQueries=" << pathQueries
                          << " dynamicSources=" << dynamicSources.size()
                          << std::endl;
            }
        }
        
        std::unique_lock lock(gameDetectionWaitMutex_);
        gameDetectionWaitCv_.wait_for(lock, std::chrono::seconds(2), [this] {
            return !gameDetectionRunning_.load();
        });
    }
}

const Diagnostics& Engine::diagnostics() {
    refreshPacketCounts();
    return diagnostics_;
}

std::string Engine::runningProcessesJson(bool includeExecutablePaths) const {
    const auto snapshot = RunningProcessSnapshot::captureNameOnly();
    std::ostringstream json;
    json << "[";
    bool wroteProcess = false;
    for (const auto& process : snapshot.entries()) {
        if (process.exeName.empty() || process.processId == 0) continue;
        if (wroteProcess) json << ",";
        wroteProcess = true;
        json << "{"
             << "\"name\":\"" << jsonEscape(process.exeName) << "\","
             << "\"pid\":" << process.processId;
        if (includeExecutablePaths) {
            json << ",\"executablePath\":\""
                 << jsonEscape(RunningProcessSnapshot::queryProcessPath(process.processId)) << "\"";
        }
        json << "}";
    }
    json << "]";
    return json.str();
}

std::string Engine::processExecutablePathJson(uint32_t processId) const {
    return "\"" + jsonEscape(RunningProcessSnapshot::queryProcessPath(processId)) + "\"";
}

const Diagnostics& Engine::configure(const EngineSettings& settings) {
    const auto previousMonitorId = settings_.monitorId;
    const auto previousTargetWidth = settings_.targetWidth;
    const auto previousTargetHeight = settings_.targetHeight;
    const auto previousCaptureGameAudio = settings_.captureGameAudio;
    const auto previousCaptureForegroundSystemAudio = settings_.captureForegroundSystemAudio;
    const auto previousMicVolume = settings_.micVolume;
    const auto previousMicIsolation = settings_.micIsolation;
    const auto previousMicIsolationWeight = settings_.micIsolationWeight;
    const auto previousNoiseGateEnabled = settings_.noiseGateEnabled;
    const auto previousAutoNoiseGate = settings_.autoNoiseGate;
    const auto previousNoiseGateThreshold = settings_.noiseGateThreshold;
    const auto previousNoiseGateDebounceMs = settings_.noiseGateDebounceMs;
    const auto previousMicDeviceId = settings_.micDeviceId;
    const auto previousMicDeviceMatchKey = settings_.micDeviceMatchKey;
    const auto previousMicDeviceName = settings_.micDeviceName;
    const auto previousAppAudioProcesses = settings_.appAudioProcesses;
    const auto previousSystemAudioProcesses = settings_.systemAudioProcesses;

    settings_.fps = std::clamp(settings.fps, 24, 240);
    settings_.bitrateMbps = std::clamp(settings.bitrateMbps, 4, 120);
    settings_.nvencPreset = std::clamp(settings.nvencPreset, 1, 5);
    settings_.clipLengthSeconds = std::clamp(settings.clipLengthSeconds, 5, 600);
    settings_.monitorId = settings.monitorId.empty() ? "primary" : settings.monitorId;
    settings_.targetWidth = std::max(0, settings.targetWidth);
    settings_.targetHeight = std::max(0, settings.targetHeight);
    const bool videoOutputTargetChanged =
        previousTargetWidth != settings_.targetWidth ||
        previousTargetHeight != settings_.targetHeight;
    if (videoOutputTargetChanged) {
        videoPackets_.clear();
        if (videoReplayStore_) videoReplayStore_->clear();
    }
    if (settings_.targetWidth > 0 && settings_.targetHeight > 0) {
        pendingAutoVideoResolutionReset100ns_ = 0;
    }
    settings_.includeMixedAudio = settings.includeMixedAudio;
    settings_.includeSystemAudio = settings.includeSystemAudio;
    settings_.includeMicrophoneAudio = settings.includeMicrophoneAudio;
    settings_.captureGameAudio = settings.captureGameAudio;
    settings_.captureForegroundSystemAudio = settings.captureForegroundSystemAudio;
    if (!settings_.captureForegroundSystemAudio && !settings_.captureGameAudio) {
        foregroundSystemProcesses_.clear();
    }
    settings_.micVolume = settings.micVolume;
    settings_.micIsolation = settings.micIsolation;
    settings_.micIsolationWeight = settings.micIsolationWeight;
    settings_.noiseGateEnabled = settings.noiseGateEnabled;
    settings_.autoNoiseGate = settings.autoNoiseGate;
    settings_.noiseGateThreshold = settings.noiseGateThreshold;
    settings_.noiseGateDebounceMs = settings.noiseGateDebounceMs;
    settings_.micDeviceId = settings.micDeviceId;
    settings_.micDeviceMatchKey = settings.micDeviceMatchKey;
    settings_.micDeviceName = settings.micDeviceName;
    settings_.appAudioProcesses = settings.appAudioProcesses;
    settings_.systemAudioProcesses = settings.systemAudioProcesses;
    if (encoderWorker_) {
        const auto [maxEncodeWidth, maxEncodeHeight] = encoderMaxDimensions(settings_);
        encoderWorker_->configure(
            settings_.fps,
            settings_.bitrateMbps,
            settings_.targetWidth,
            settings_.targetHeight,
            maxEncodeWidth,
            maxEncodeHeight,
            settings_.nvencPreset,
            videoOutputTargetChanged);
    }
    if (captureSession_) {
        captureSession_->setTargetFps(settings_.fps);
        if (captureSession_->running() && previousMonitorId != settings_.monitorId) {
            if (encoderWorker_) {
                if (settings_.targetWidth <= 0 || settings_.targetHeight <= 0) {
                    videoPackets_.clear();
                    if (videoReplayStore_) videoReplayStore_->clear();
                    encoderWorker_->resetAutoOutputResolution();
                } else {
                    encoderWorker_->requireFreshFrame(true);
                }
            }
            const bool captureStarted = captureSession_->startMonitor(&frameQueue_, &captureTickGate_, settings_.monitorId);
            diagnostics_.captureReady = captureStarted;
            diagnostics_.degraded = !diagnostics_.hardwareAcceleration || !captureStarted;
            diagnostics_.status = captureSession_->status();
            if (captureStarted) {
                diagnostics_.resolution = captureSession_->resolution();
                diagnostics_.display = captureSession_->displayName();
                diagnostics_.hdrTonemapping = captureSession_->hdrTonemappingActive();
            }
        }
    }
    const bool appAudioChanged =
        previousCaptureGameAudio != settings_.captureGameAudio ||
        previousCaptureForegroundSystemAudio != settings_.captureForegroundSystemAudio ||
        previousAppAudioProcesses != settings_.appAudioProcesses ||
        previousSystemAudioProcesses != settings_.systemAudioProcesses;
    const bool micSettingsChanged =
        previousMicVolume != settings_.micVolume ||
        previousMicIsolation != settings_.micIsolation ||
        previousMicIsolationWeight != settings_.micIsolationWeight ||
        previousNoiseGateEnabled != settings_.noiseGateEnabled ||
        previousAutoNoiseGate != settings_.autoNoiseGate ||
        previousNoiseGateThreshold != settings_.noiseGateThreshold ||
        previousNoiseGateDebounceMs != settings_.noiseGateDebounceMs ||
        previousMicDeviceId != settings_.micDeviceId ||
        previousMicDeviceMatchKey != settings_.micDeviceMatchKey ||
        previousMicDeviceName != settings_.micDeviceName;

    if (audioCaptureWorker_ && appAudioChanged) {
        {
            std::lock_guard<std::mutex> lock(appAudioSourcesMutex_);
            activeAppAudioSources_ = settings_.appAudioProcesses;
            for (const auto& systemProcess : settings_.systemAudioProcesses) {
                if (!containsProcessName(activeAppAudioSources_, systemProcess)) {
                    activeAppAudioSources_.push_back(systemProcess);
                }
            }
            activeAppAudioTrackAliases_.clear();
            for (const auto& systemProcess : settings_.systemAudioProcesses) {
                activeAppAudioTrackAliases_[sourceIdForProcessSpec(systemProcess)] = "system-loopback-pcm";
            }
        }
        std::vector<std::string> captureSources = settings_.appAudioProcesses;
        for (const auto& systemProcess : settings_.systemAudioProcesses) {
            if (!containsProcessName(captureSources, systemProcess)) captureSources.push_back(systemProcess);
        }
        audioCaptureWorker_->configureAppSources(captureSources);
    }
    refreshAudioRouting();
    if (audioCaptureWorker_ && micSettingsChanged) {
        audioCaptureWorker_->setMicSettings(settings_.micVolume, settings_.micIsolation, settings_.micIsolationWeight, settings_.noiseGateEnabled, settings_.autoNoiseGate, settings_.noiseGateThreshold, settings_.noiseGateDebounceMs, settings_.micDeviceId, settings_.micDeviceMatchKey, settings_.micDeviceName);
    }

    diagnostics_.fps = settings_.fps;
    diagnostics_.bitrateMbps = settings_.bitrateMbps;
    diagnostics_.bufferDurationSeconds = settings_.clipLengthSeconds;
    diagnostics_.encoderMode = "NVENC P" + std::to_string(settings_.nvencPreset) + " (buffered sync preferred; async compatibility fallback)";
    diagnostics_.microphoneDevice = audioCaptureWorker_ ? audioCaptureWorker_->microphoneStatus() : microphoneDeviceName(settings_.micDeviceId);
    diagnostics_.clipTargetResolution = settings_.targetWidth > 0 && settings_.targetHeight > 0
        ? formatResolution(settings_.targetWidth, settings_.targetHeight)
        : "Clip-aware system";

    const auto retention100ns = static_cast<int64_t>(settings_.clipLengthSeconds + 5) * 10'000'000LL;
    const auto replayBudgets = replayMemoryBudgets(settings_);
    videoPackets_.setRetention(kHotReplayRetention100ns);
    if (videoReplayStore_) {
        videoReplayStore_->setRetention(retention100ns);
        videoReplayStore_->setResidentPayloadBudget(replayBudgets.videoBytes);
    }
    if (aacReplayStore_) {
        aacReplayStore_->setRetention(retention100ns);
        aacReplayStore_->setResidentPayloadBudget(replayBudgets.audioBytes);
    }
    if (pcmRecoveryStore_) {
        pcmRecoveryStore_->setRetention(retention100ns);
        pcmRecoveryStore_->setResidentPayloadBudget(replayBudgets.pcmRecoveryBytes);
    }
    if (audioReplayCoordinator_) {
        audioReplayCoordinator_->setRetention(retention100ns);
        aacAudioPackets_.setRetention(kHotReplayRetention100ns);
    } else {
        audioPackets_.setRetention(retention100ns);
        aacAudioPackets_.setRetention(kHotReplayRetention100ns);
    }
    if (!armed_) {
        armed_ = true;
        arm();
        gameDetectionRunning_ = true;
        gameDetectionThread_ = std::thread([this] { gameDetectionLoop(); });
    }
    refreshPacketCounts();
    return diagnostics_;
}

std::vector<EncodedPacket> mixPcmPackets(
    const std::vector<const EncodedPacket*>& packets,
    const std::string& newSourceId,
    int64_t clipStart,
    int64_t clipEnd,
    std::vector<float>& mixScratch) {
    if (packets.empty()) return {};

    int sampleRate = 0;
    int channels = 0;

    for (const auto* packet : packets) {
        if (!packet) continue;
        if (sampleRate == 0) sampleRate = packet->sampleRate;
        if (channels == 0) channels = packet->channelCount;
    }

    if (clipStart >= clipEnd || sampleRate == 0 || channels == 0) return {};

    struct PcmInputView {
        const EncodedPacket* packet = nullptr;
        int64_t startFrame = 0;
        int64_t endFrame = 0;
        int frames = 0;
    };

    const int bytesPerFrame = channels * static_cast<int>(sizeof(int16_t));
    auto timelineFrameFrom100ns = [sampleRate](int64_t offset100ns) {
        return (offset100ns * sampleRate) / 10'000'000LL;
    };

    const int64_t totalDuration = clipEnd - clipStart;
    const int64_t totalSamples = timelineFrameFrom100ns(totalDuration);
    if (totalSamples <= 0) return {};

    std::vector<PcmInputView> inputs;
    inputs.reserve(packets.size());
    bool hasAudio = false;
    for (const auto* packet : packets) {
        if (!packet) continue;
        const auto& p = *packet;
        if (p.sampleRate != sampleRate || p.channelCount != channels) continue;
        if (!p.audible || payloadEmpty(p)) continue;

        const int frames = static_cast<int>(payloadSize(p) / static_cast<std::size_t>(bytesPerFrame));
        if (frames <= 0) continue;
        const int64_t startFrame = timelineFrameFrom100ns(p.pts100ns - clipStart);
        const int64_t endFrame = startFrame + frames;
        if (endFrame <= 0 || startFrame >= totalSamples) continue;

        hasAudio = true;
        inputs.push_back(PcmInputView { &p, startFrame, endFrame, frames });
    }

    if (inputs.empty() || !hasAudio) {
        return {};
    }

    std::sort(inputs.begin(), inputs.end(), [](const PcmInputView& a, const PcmInputView& b) {
        return a.startFrame < b.startFrame;
    });

    std::vector<EncodedPacket> mixed;
    const int framesPerPacket = std::max(1, sampleRate / 100);
    mixed.reserve(static_cast<std::size_t>((totalSamples + framesPerPacket - 1) / framesPerPacket));
    mixScratch.resize(static_cast<std::size_t>(framesPerPacket) * static_cast<std::size_t>(channels));
    std::vector<std::byte> inputReadScratch;

    std::size_t firstCandidate = 0;
    for (int64_t sampleOffset = 0; sampleOffset < totalSamples; sampleOffset += framesPerPacket) {
        const int frames = static_cast<int>(std::min<int64_t>(framesPerPacket, totalSamples - sampleOffset));
        const std::size_t scratchSamples = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);
        std::fill(mixScratch.begin(), mixScratch.begin() + static_cast<std::ptrdiff_t>(scratchSamples), 0.0f);

        const int64_t chunkStart100ns = clipStart + (sampleOffset * 10'000'000LL) / sampleRate;

        while (firstCandidate < inputs.size() &&
               inputs[firstCandidate].endFrame <= sampleOffset) {
            ++firstCandidate;
        }

        for (std::size_t inputIndex = firstCandidate; inputIndex < inputs.size(); ++inputIndex) {
            const auto& input = inputs[inputIndex];
            if (input.startFrame >= sampleOffset + frames) break;

            const int64_t overlapStartFrame = std::max(sampleOffset, input.startFrame);
            const int64_t overlapEndFrame = std::min(sampleOffset + frames, input.endFrame);
            if (overlapEndFrame <= overlapStartFrame) continue;

            const int64_t outputStartFrame = overlapStartFrame - sampleOffset;
            const int64_t inputStartFrame = overlapStartFrame - input.startFrame;
            const int64_t copyFrames = std::min<int64_t>({
                overlapEndFrame - overlapStartFrame,
                frames - outputStartFrame,
                input.frames - inputStartFrame
            });
            if (copyFrames <= 0) continue;

            const std::size_t inputByteOffset =
                static_cast<std::size_t>(inputStartFrame) * static_cast<std::size_t>(bytesPerFrame);
            const std::size_t inputByteCount =
                static_cast<std::size_t>(copyFrames) * static_cast<std::size_t>(bytesPerFrame);
            const int16_t* inputSamples = nullptr;
            const auto residentBytes = payloadBytes(*input.packet);
            if (!residentBytes.empty()) {
                if (inputByteOffset > residentBytes.size() ||
                    inputByteCount > residentBytes.size() - inputByteOffset) {
                    continue;
                }
                inputSamples = reinterpret_cast<const int16_t*>(residentBytes.data() + inputByteOffset);
            } else {
                inputReadScratch.resize(inputByteCount);
                if (!readPayload(*input.packet, inputByteOffset, inputReadScratch)) continue;
                inputSamples = reinterpret_cast<const int16_t*>(inputReadScratch.data());
            }

            for (int64_t frame = 0; frame < copyFrames; ++frame) {
                for (int ch = 0; ch < channels; ++ch) {
                    const std::size_t outIndex = static_cast<std::size_t>((outputStartFrame + frame) * channels + ch);
                    const std::size_t inIndex = static_cast<std::size_t>(frame * channels + ch);
                    mixScratch[outIndex] += inputSamples[inIndex];
                }
            }
        }

        float peak = 0.0f;
        for (std::size_t sample = 0; sample < scratchSamples; ++sample) {
            peak = std::max(peak, std::abs(mixScratch[sample]));
        }
        const float limiterGain = peak > 32767.0f ? 32767.0f / peak : 1.0f;

        EncodedPacket mp;
        mp.kind = PacketKind::Audio;
        mp.codec = PacketCodec::PcmS16;
        mp.pts100ns = chunkStart100ns;
        mp.dts100ns = mp.pts100ns;
        mp.duration100ns = (frames * 10'000'000) / sampleRate;
        mp.sourceId = newSourceId;
        mp.logicalTrackId = newSourceId;
        mp.encoderId = "PCM_S16";
        mp.sampleRate = sampleRate;
        mp.channelCount = channels;
        mp.bitsPerSample = 16;
        mp.audioFrameCount = static_cast<uint32_t>(frames);
        mp.audible = true;
        mp.payload = std::make_shared<PacketPayload>(frames * channels * sizeof(int16_t));
        
        int16_t* outPcm = reinterpret_cast<int16_t*>(mutablePayload(mp).data());
        for (int j = 0; j < frames; ++j) {
            for (int ch = 0; ch < channels; ++ch) {
                float val = mixScratch[static_cast<std::size_t>(j * channels + ch)] * limiterGain;
                outPcm[j * channels + ch] = static_cast<int16_t>(std::clamp(val, -32768.0f, 32767.0f));
            }
        }
        mixed.push_back(std::move(mp));
    }
    return mixed;
}

SaveClipResult Engine::saveClip(const SaveClipRequest& request) {
    SaveClipResult result;
    diagnostics_.lastClipCadence = {};
    const auto totalStartedAt = SaveTimingClock::now();
    logEngineSaveTiming("start", totalStartedAt, "durationSeconds=" + std::to_string(request.durationSeconds));
    auto currentVideoDropCount = [this]() {
        uint64_t total = frameQueue_.stats().overflowDrops;
        if (captureSession_) total += captureSession_->ownedSlotDrops();
        if (encoderWorker_) {
            total += static_cast<uint64_t>(std::max(0, encoderWorker_->schedulerDroppedFrames()));
            total += static_cast<uint64_t>(std::max(0, encoderWorker_->encoderBackpressureDrops()));
        }
        return static_cast<int>(std::min<uint64_t>(total, static_cast<uint64_t>(std::numeric_limits<int>::max())));
    };
    const int saveStartDroppedFrames = currentVideoDropCount();
    const auto saveStartFrameQueueStats = frameQueue_.stats();
    const auto saveStartCaptureStats = captureSession_
        ? captureSession_->runtimeStats()
        : CaptureRuntimeStats {};
    const uint64_t saveStartSourceSuperseded = captureSession_ ? captureSession_->sourceFramesSuperseded() : 0;
    const uint64_t saveStartCaptureSlotDrops = captureSession_ ? captureSession_->ownedSlotDrops() : 0;
    const int saveStartSchedulerSkips = encoderWorker_ ? encoderWorker_->schedulerDroppedFrames() : 0;
    const int saveStartSchedulerRepeats = encoderWorker_ ? encoderWorker_->schedulerRepeatedFrames() : 0;
    const int saveStartEncoderQueueDrops = encoderWorker_ ? encoderWorker_->encoderQueueDrops() : 0;
    const int saveStartNvencSurfaceDrops = encoderWorker_ ? encoderWorker_->nvencSurfaceDrops() : 0;
    const int saveStartNvencInputDrops = encoderWorker_ ? encoderWorker_->nvencInputDrops() : 0;
    const int saveStartEncoderAccepted = encoderWorker_ ? encoderWorker_->framesAccepted() : 0;
    const int saveStartEncoderEncoded = encoderWorker_ ? encoderWorker_->framesEncoded() : 0;
    const int saveStartFrameQueueDepth = static_cast<int>(frameQueue_.size());
    const int64_t saveStartOldestFrameAge100ns = frameQueue_.oldestFrameAge100ns();
    const int saveStartEncoderQueueDepth = encoderWorker_ ? encoderWorker_->queuedFreshEncodeFrames() : 0;
    const int saveStartNvencInFlight = encoderWorker_ ? encoderWorker_->nvencInFlightFrames() : 0;
    const int64_t saveStartCaptureGap100ns = captureSession_ ? captureSession_->lastFrameInterval100ns() : 0;
    const int64_t saveStartCapturePublicationAge100ns = captureSession_
        ? captureSession_->lastPublishedAge100ns()
        : 0;
    auto saveStutterDeltaDetails = [&]() {
        const int currentDroppedFrames = currentVideoDropCount();
        const int currentEncoderAccepted = encoderWorker_ ? encoderWorker_->framesAccepted() : 0;
        const int currentEncoderEncoded = encoderWorker_ ? encoderWorker_->framesEncoded() : 0;
        const auto currentFrameQueueStats = frameQueue_.stats();
        const auto currentCaptureStats = captureSession_
            ? captureSession_->runtimeStats()
            : CaptureRuntimeStats {};
        return " droppedFramesDelta=" + std::to_string(currentDroppedFrames - saveStartDroppedFrames) +
            " captureBackend=\"" + jsonEscape(currentCaptureStats.activeBackend) + "\"" +
            " acquiredUpdateDelta=" + std::to_string(
                currentCaptureStats.acquiredUpdates - saveStartCaptureStats.acquiredUpdates) +
            " accumulationEventDelta=" + std::to_string(
                currentCaptureStats.accumulationEvents - saveStartCaptureStats.accumulationEvents) +
            " accumulatedFrameDelta=" + std::to_string(
                currentCaptureStats.accumulatedFrames - saveStartCaptureStats.accumulatedFrames) +
            " captureOverflowDelta=" + std::to_string(
                currentFrameQueueStats.overflowDrops - saveStartFrameQueueStats.overflowDrops) +
            " sourceSupersededDelta=" + std::to_string(
                (captureSession_ ? captureSession_->sourceFramesSuperseded() : 0) - saveStartSourceSuperseded) +
            " captureSlotDropDelta=" + std::to_string(
                (captureSession_ ? captureSession_->ownedSlotDrops() : 0) - saveStartCaptureSlotDrops) +
            " schedulerSkipDelta=" + std::to_string(
                (encoderWorker_ ? encoderWorker_->schedulerDroppedFrames() : 0) - saveStartSchedulerSkips) +
            " schedulerRepeatDelta=" + std::to_string(
                (encoderWorker_ ? encoderWorker_->schedulerRepeatedFrames() : 0) - saveStartSchedulerRepeats) +
            " encoderQueueDropDelta=" + std::to_string(
                (encoderWorker_ ? encoderWorker_->encoderQueueDrops() : 0) - saveStartEncoderQueueDrops) +
            " nvencSurfaceDropDelta=" + std::to_string(
                (encoderWorker_ ? encoderWorker_->nvencSurfaceDrops() : 0) - saveStartNvencSurfaceDrops) +
            " nvencInputDropDelta=" + std::to_string(
                (encoderWorker_ ? encoderWorker_->nvencInputDrops() : 0) - saveStartNvencInputDrops) +
            " encoderAcceptedDelta=" + std::to_string(currentEncoderAccepted - saveStartEncoderAccepted) +
            " encoderEncodedDelta=" + std::to_string(currentEncoderEncoded - saveStartEncoderEncoded);
    };
    auto saveTotalDetails = [&](const std::string& details) {
        return details + saveStutterDeltaDetails();
    };
    MuxWritePacing savePacing {
        [this,
         saveStartDroppedFrames,
         saveStartFrameQueueDepth,
         saveStartOldestFrameAge100ns,
         saveStartEncoderQueueDepth,
         saveStartNvencInFlight,
         saveStartCaptureGap100ns,
         saveStartCapturePublicationAge100ns,
         currentVideoDropCount,
         previousDroppedFrames = saveStartDroppedFrames,
         previousPublishedFrames = saveStartCaptureStats.publishedFrames,
         previousDesktopPresents = saveStartCaptureStats.desktopPresents]() mutable {
            MuxPressureSample sample;
            sample.frameQueueDepth = frameQueue_.size();
            sample.oldestFrameAge100ns = frameQueue_.oldestFrameAge100ns();
            sample.encoderQueueDepth = encoderWorker_ ? encoderWorker_->queuedFreshEncodeFrames() : 0;
            sample.nvencInFlight = encoderWorker_ ? encoderWorker_->nvencInFlightFrames() : 0;
            const auto captureStats = captureSession_
                ? captureSession_->runtimeStats()
                : CaptureRuntimeStats {};
            sample.captureGap100ns = captureSession_ ? captureSession_->lastFrameInterval100ns() : 0;
            sample.capturePublicationAge100ns = captureSession_
                ? captureSession_->lastPublishedAge100ns()
                : 0;
            const bool captureWasActive =
                captureStats.publishedFrames > previousPublishedFrames ||
                captureStats.desktopPresents > previousDesktopPresents;
            previousPublishedFrames = captureStats.publishedFrames;
            previousDesktopPresents = captureStats.desktopPresents;
            const int currentDroppedFrames = currentVideoDropCount();
            const bool newFrameDrop = currentDroppedFrames > previousDroppedFrames;
            previousDroppedFrames = currentDroppedFrames;
            sample.droppedFramesDelta = currentDroppedFrames - saveStartDroppedFrames;
            const int64_t frameInterval100ns = 10'000'000LL / std::max(1, diagnostics_.fps);
            const auto pressure = classifyCaptureWritePressure({
                sample.frameQueueDepth,
                sample.oldestFrameAge100ns,
                sample.encoderQueueDepth,
                sample.nvencInFlight,
                sample.captureGap100ns,
                sample.capturePublicationAge100ns,
                frameInterval100ns,
                captureWasActive,
                newFrameDrop,
            });
            if (pressure == AdaptiveWritePressure::Critical) {
                sample.level = MuxPressureLevel::Critical;
            } else if (pressure == AdaptiveWritePressure::Elevated) {
                sample.level = MuxPressureLevel::Elevated;
            }
            return sample;
        }
    };
    savePacing.burstBytes = kSaveWriteBurstBytes;
    savePacing.storageAwareRate = true;
    savePacing.analyzeIo = request.analyzeIo;
    logEngineSaveTiming(
        "stutter_baseline",
        totalStartedAt,
        "droppedFrames=" + std::to_string(saveStartDroppedFrames) +
            " encoderAccepted=" + std::to_string(saveStartEncoderAccepted) +
            " encoderEncoded=" + std::to_string(saveStartEncoderEncoded) +
            " frameQueueDepth=" + std::to_string(saveStartFrameQueueDepth) +
            " oldestFrameAge100ns=" + std::to_string(saveStartOldestFrameAge100ns) +
            " encoderQueueDepth=" + std::to_string(saveStartEncoderQueueDepth) +
            " nvencInFlight=" + std::to_string(saveStartNvencInFlight) +
            " captureGap100ns=" + std::to_string(saveStartCaptureGap100ns) +
            " capturePublicationAge100ns=" + std::to_string(saveStartCapturePublicationAge100ns));

    if (diagnostics_.activeEncoder != EncoderName::Nvenc) {
        result.message = "Cannot save clip: direct NVENC is not available, and this MVP requires NVENC first.";
        logEngineSaveTiming("total", totalStartedAt, saveTotalDetails("ok=false reason=nvenc_unavailable"));
        return result;
    }

    const std::size_t bufferedVideoPacketCount = videoReplayStore_
        ? videoReplayStore_->size()
        : videoPackets_.size();
    if (bufferedVideoPacketCount == 0) {
        result.message = "No encoded H.264 packets are buffered yet. Keep diagnostics open and wait for Encoder packets to rise above 0.";
        logEngineSaveTiming("total", totalStartedAt, saveTotalDetails("ok=false reason=no_video_packets"));
        return result;
    }

    const int duration = std::clamp(request.durationSeconds, 5, 600);
    const auto snapshotStartedAt = SaveTimingClock::now();
    auto videoSnapshot = videoReplayStore_
        ? videoReplayStore_->snapshot()
        : videoPackets_.snapshot();
    const auto diskBackedVideoPackets = std::count_if(
        videoSnapshot.begin(),
        videoSnapshot.end(),
        [](const EncodedPacket& packet) { return !packet.payload && packet.payloadReader; });
    const auto videoArchiveStats = videoReplayStore_
        ? videoReplayStore_->stats()
        : ReplaySegmentStoreStats {};
    logEngineSaveTiming(
        "video_snapshot",
        snapshotStartedAt,
        "packets=" + std::to_string(videoSnapshot.size()) +
            " diskBacked=" + std::to_string(diskBackedVideoPackets) +
            " queued=" + std::to_string(videoArchiveStats.queuedPackets) +
            " queuedBytes=" + std::to_string(videoArchiveStats.queuedBytes) +
            " residentBytes=" + std::to_string(videoArchiveStats.residentPayloadBytes) +
            " residentBudgetBytes=" + std::to_string(videoArchiveStats.residentPayloadBudgetBytes) +
            " ramFallbackBytes=" + std::to_string(videoArchiveStats.ramFallbackBytes));

    const auto videoSelectStartedAt = SaveTimingClock::now();
    auto clipPackets = selectVideoWindowForClip(
        std::move(videoSnapshot),
        duration,
        mediaNow100ns());
    logEngineSaveTiming("video_select", videoSelectStartedAt, "clipPackets=" + std::to_string(clipPackets.size()));
    if (clipPackets.empty()) {
        result.message = "No complete keyframe-starting H.264 window is buffered yet. Wait about one second and try again.";
        logEngineSaveTiming("total", totalStartedAt, saveTotalDetails("ok=false reason=no_keyframe_window"));
        return result;
    }
    const int actualDuration = actualClipDurationSeconds(clipPackets, duration);
    diagnostics_.lastClipCadence = analyzeVideoCadence(
        std::span<const EncodedPacket>(clipPackets.data(), clipPackets.size()),
        diagnostics_.fps);
    if (clipPackets.size() >= 2) {
        const auto videoTimelineStartedAt = SaveTimingClock::now();
        const int64_t targetFrameDuration100ns = 10'000'000LL / std::max(1, diagnostics_.fps);
        int64_t maxGap100ns = 0;
        std::size_t longVfrGaps = 0;
        uint64_t unfilledTargetIntervals = 0;
        uint64_t uniqueSourceSequences = 0;
        uint64_t repeatedSourceTicks = 0;
        uint64_t previousSourceSequence = 0;
        for (const auto& packet : clipPackets) {
            if (packet.sourceFrameSequence == 0) continue;
            if (packet.sourceFrameSequence == previousSourceSequence) {
                ++repeatedSourceTicks;
            } else {
                ++uniqueSourceSequences;
                previousSourceSequence = packet.sourceFrameSequence;
            }
        }
        for (std::size_t i = 1; i < clipPackets.size(); ++i) {
            const int64_t gap100ns = std::max<int64_t>(1, clipPackets[i].pts100ns - clipPackets[i - 1].pts100ns);
            maxGap100ns = std::max(maxGap100ns, gap100ns);
            const int64_t missing = std::max<int64_t>(
                0,
                (gap100ns + targetFrameDuration100ns / 2) / targetFrameDuration100ns - 1);
            if (missing > 0) {
                ++longVfrGaps;
                unfilledTargetIntervals += static_cast<uint64_t>(missing);
            }
        }
        const int64_t span100ns = std::max<int64_t>(
            0,
            clipPackets.back().pts100ns - clipPackets.front().pts100ns +
                std::max<int64_t>(clipPackets.back().duration100ns, targetFrameDuration100ns));
        const uint64_t expectedOutputTicks = static_cast<uint64_t>(
            (span100ns + targetFrameDuration100ns / 2) / targetFrameDuration100ns);
        const double spanSeconds = static_cast<double>(span100ns) / 10'000'000.0;
        const double freshSourceFps = spanSeconds > 0.0
            ? static_cast<double>(uniqueSourceSequences) / spanSeconds
            : 0.0;
        const double repeatRatio = clipPackets.empty()
            ? 0.0
            : static_cast<double>(repeatedSourceTicks) / clipPackets.size();
        const auto captureStats = captureSession_
            ? captureSession_->runtimeStats()
            : CaptureRuntimeStats {};
        const auto encoderRecent = encoderWorker_
            ? encoderWorker_->recentPerformance()
            : EncoderRecentPerformance {};
        logEngineSaveTiming(
            "video_timeline",
            videoTimelineStartedAt,
            "span100ns=" + std::to_string(span100ns) +
                " targetFrame100ns=" + std::to_string(targetFrameDuration100ns) +
                " maxGap100ns=" + std::to_string(maxGap100ns) +
                " longVfrGaps=" + std::to_string(longVfrGaps) +
                " unfilledTargetIntervals=" + std::to_string(unfilledTargetIntervals) +
                " expectedOutputTicks=" + std::to_string(expectedOutputTicks) +
                " encodedPackets=" + std::to_string(clipPackets.size()) +
                " uniqueSourceSequences=" + std::to_string(uniqueSourceSequences) +
                " repeatedSourceTicks=" + std::to_string(repeatedSourceTicks) +
                " freshSourceFps=" + fixedNumber(freshSourceFps) +
                " repeatRatio=" + fixedNumber(repeatRatio, 4) +
                " worstSecondFreshSourceFps=" + fixedNumber(
                    diagnostics_.lastClipCadence.worstSecondDistinctSourceFps) +
                " desktopPresentSourceFps=" + fixedNumber(
                    diagnostics_.lastClipCadence.desktopPresentSourceFps) +
                " pointerOnlySourceFrames=" + std::to_string(
                    diagnostics_.lastClipCadence.pointerOnlySourceFrames) +
                " underTargetSeconds=" + std::to_string(
                    diagnostics_.lastClipCadence.underTargetSeconds) +
                " longestHeldRunSamples=" + std::to_string(
                    diagnostics_.lastClipCadence.longestHeldRunSamples) +
                " stillFrameDuplication=" + std::string(
                    diagnostics_.stillFrameDuplicationEnabled ? "true" : "false") +
                " captureBackend=\"" + jsonEscape(captureStats.activeBackend) + "\"" +
                " desktopPresentFps=" + fixedNumber(captureStats.desktopPresentFps) +
                " publishedFreshFps=" + fixedNumber(captureStats.publishedFreshFps) +
                " recentPublishedFreshFps=" + fixedNumber(captureStats.recentPublishedFreshFps) +
                " recentEncoderInputFps=" + fixedNumber(encoderRecent.inputFps) +
                " recentEncoderOutputFps=" + fixedNumber(encoderRecent.outputFps) +
                " capturePublicationAge100ns=" + std::to_string(
                    captureSession_ ? captureSession_->lastPublishedAge100ns() : 0) +
                " hdrTonemapping=" + std::string(
                    captureSession_ && captureSession_->hdrTonemappingActive() ? "true" : "false") +
                " encoderQueueDropsTotal=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->encoderQueueDrops() : 0) +
                " configuredNvencPreset=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->effectiveNvencPreset() : 0) +
                " nvencSurfaceDropsTotal=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->nvencSurfaceDrops() : 0) +
                " averageScaleLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->averageScaleLatency100ns() : 0) +
                " maximumScaleLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->maximumScaleLatency100ns() : 0) +
                " averageInputMapLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->averageInputMapLatency100ns() : 0) +
                " maximumInputMapLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->maximumInputMapLatency100ns() : 0) +
                " averageNvencCallLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->averageNvencCallLatency100ns() : 0) +
                " maximumNvencCallLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->maximumNvencCallLatency100ns() : 0) +
                " averageOutputDrainLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->averageOutputDrainLatency100ns() : 0) +
                " maximumOutputDrainLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->maximumOutputDrainLatency100ns() : 0) +
                " capturePreparationP95_100ns=" + std::to_string(
                    captureStats.framePreparationLatency.p95_100ns) +
                " captureProcessingP95_100ns=" + std::to_string(
                    captureStats.frameProcessingLatency.p95_100ns) +
                " inputPreparationP95_100ns=" + std::to_string(
                    encoderRecent.inputPreparation.p95_100ns) +
                " inputMapP95_100ns=" + std::to_string(encoderRecent.inputMap.p95_100ns) +
                " nvencCallP95_100ns=" + std::to_string(encoderRecent.encodeCall.p95_100ns) +
                " outputEventWaitP95_100ns=" + std::to_string(
                    encoderRecent.outputEventWait.p95_100ns) +
                " outputLockP95_100ns=" + std::to_string(encoderRecent.outputLock.p95_100ns) +
                " outputCopyP95_100ns=" + std::to_string(encoderRecent.outputCopy.p95_100ns) +
                " outputUnmapP95_100ns=" + std::to_string(encoderRecent.outputUnmap.p95_100ns) +
                " nvencZeroCopyFrames=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->nvencZeroCopyFrames() : 0) +
                " nvencCopyFallbackFrames=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->nvencCopyFallbackFrames() : 0) +
                " nvencConvertedFrames=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->nvencConvertedFrames() : 0) +
                " maximumSubmitLatency100ns=" + std::to_string(
                    encoderWorker_ ? encoderWorker_->maximumSubmitLatency100ns() : 0));
    }

    auto [width, height] = encodedResolutionFromPackets(clipPackets);
    if (width <= 0 || height <= 0) {
        const auto fallbackResolution = parseResolution(diagnostics_.resolution);
        width = fallbackResolution.first;
        height = fallbackResolution.second;
    }
    const auto clipResolution = formatResolution(width, height);
    auto [recommendedWidth, recommendedHeight] = lowestSourceResolutionFromPackets(clipPackets);
    if (recommendedWidth <= 0 || recommendedHeight <= 0) {
        recommendedWidth = width;
        recommendedHeight = height;
    }
    const auto recommendedResolution = formatResolution(recommendedWidth, recommendedHeight);
    const int64_t clipStart = clipPackets.front().pts100ns;
    const int64_t clipEnd = clipPackets.back().pts100ns + std::max<int64_t>(clipPackets.back().duration100ns, 0);
    const int64_t requestedClipStart = std::max<int64_t>(
        clipStart,
        clipEnd - static_cast<int64_t>(duration) * 10'000'000LL);
    savePacing.presentationStartPts100ns = requestedClipStart;
    savePacing.presentationEndPts100ns = clipEnd;
    const auto audioSelectStartedAt = SaveTimingClock::now();
    constexpr int64_t kAudioFrameLookbehind100ns = 2'000'000LL;
    const int64_t audioSelectionStart = std::max<int64_t>(0, clipStart - kAudioFrameLookbehind100ns);
    const int64_t liveAudioTarget100ns = std::max<int64_t>(
        audioSelectionStart,
        clipEnd - kAudioFrameLookbehind100ns);
    const bool liveAudioCaughtUp = audioReplayCoordinator_ &&
        audioReplayCoordinator_->waitUntil(liveAudioTarget100ns, std::chrono::milliseconds(25));
    auto encodedAudioPackets = aacReplayStore_
        ? aacReplayStore_->selectWindow(audioSelectionStart, clipEnd)
        : aacAudioPackets_.selectWindow(audioSelectionStart, clipEnd);
    const auto replayStats = audioReplayCoordinator_ ? audioReplayCoordinator_->stats() : AudioReplayStats {};
    const bool boundedPcmTail = liveAudioCaughtUp &&
        !replayStats.pcmRecoveryActive && replayStats.committedPts100ns > 0;
    const int64_t pcmSelectionStart = boundedPcmTail
        ? std::max(audioSelectionStart, replayStats.committedPts100ns - kAudioFrameLookbehind100ns)
        : audioSelectionStart;
    auto capturedAudioPackets = audioReplayCoordinator_
        ? audioReplayCoordinator_->selectPcmWindow(pcmSelectionStart, clipEnd)
        : audioPackets_.selectWindow(pcmSelectionStart, clipEnd);
    const auto diskBackedAudioPackets = std::count_if(
        encodedAudioPackets.begin(),
        encodedAudioPackets.end(),
        [](const EncodedPacket& packet) { return !packet.payload && packet.payloadReader; });
    const auto diskBackedPcmPackets = std::count_if(
        capturedAudioPackets.begin(),
        capturedAudioPackets.end(),
        [](const EncodedPacket& packet) { return !packet.payload && packet.payloadReader; });
    logEngineSaveTiming(
        "audio_select",
        audioSelectStartedAt,
            "aacPackets=" + std::to_string(encodedAudioPackets.size()) +
            " diskBackedAac=" + std::to_string(diskBackedAudioPackets) +
            " pcmFallbackPackets=" + std::to_string(capturedAudioPackets.size()) +
            " diskBackedPcm=" + std::to_string(diskBackedPcmPackets) +
            " liveCaughtUp=" + std::string(liveAudioCaughtUp ? "true" : "false") +
            " pcmTailOnly=" + std::string(boundedPcmTail ? "true" : "false") +
            " pcmSelectionStart100ns=" + std::to_string(pcmSelectionStart) +
            " queue=" + std::to_string(replayStats.queuedPackets) +
            " queueHighWatermark=" + std::to_string(replayStats.queueHighWatermark) +
            " queueOverflows=" + std::to_string(replayStats.queueOverflows) +
            " encoderRestarts=" + std::to_string(replayStats.encoderRestarts) +
            " pcmRecoveryActive=" + std::string(replayStats.pcmRecoveryActive ? "true" : "false") +
            " committedPts100ns=" + std::to_string(replayStats.committedPts100ns));

    const auto audioGroupStartedAt = SaveTimingClock::now();
    std::map<std::string, std::string> appTrackAliases;
    {
        std::lock_guard<std::mutex> lock(appAudioSourcesMutex_);
        appTrackAliases = activeAppAudioTrackAliases_;
    }

    std::map<std::string, std::vector<const EncodedPacket*>> audioByTrack;
    for (const auto& packet : capturedAudioPackets) {
        if (!packet.logicalTrackId.empty()) {
            audioByTrack[packet.logicalTrackId].push_back(&packet);
        } else if (packet.sourceId == "system-loopback-pcm" && settings_.includeSystemAudio) {
            audioByTrack["system-loopback-pcm"].push_back(&packet);
        } else if (packet.sourceId == "microphone-pcm" && settings_.includeMicrophoneAudio) {
            audioByTrack["microphone-pcm"].push_back(&packet);
        } else if (packet.sourceId.rfind("game:", 0) == 0 && settings_.captureGameAudio) {
            std::string exeName = packet.sourceId.substr(5);
            std::string gameName = getGameName(exeName);
            if (isMinimizedRobloxName(gameName)) continue;
            audioByTrack["game:" + gameName].push_back(&packet);
        } else if (packet.sourceId.rfind("app:", 0) == 0) {
            const auto alias = appTrackAliases.find(packet.sourceId);
            audioByTrack[alias == appTrackAliases.end() ? packet.sourceId : alias->second].push_back(&packet);
        }
    }
    logEngineSaveTiming(
        "audio_group",
        audioGroupStartedAt,
        "tracks=" + std::to_string(audioByTrack.size()) +
            " packets=" + std::to_string(capturedAudioPackets.size()) +
            " processChecks=0");
    
    std::vector<EncodedPacket> normalizedAudioPackets;
    std::vector<float> audioMixScratch;
    const auto audioMixStartedAt = SaveTimingClock::now();
    std::size_t mixedAudioPacketCount = 0;
    for (const auto& [trackId, packets] : audioByTrack) {
        const auto trackMixStartedAt = SaveTimingClock::now();
        int64_t retainedStart = clipEnd;
        int64_t retainedEnd = clipStart;
        for (const auto* packet : packets) {
            if (!packet) continue;
            retainedStart = std::min(retainedStart, packet->pts100ns);
            retainedEnd = std::max(
                retainedEnd,
                packet->pts100ns + std::max<int64_t>(1, packet->duration100ns));
        }
        retainedStart = std::max(clipStart, retainedStart);
        retainedEnd = std::min(clipEnd, retainedEnd);
        auto mixedTrack = mixPcmPackets(packets, trackId, retainedStart, retainedEnd, audioMixScratch);
        logEngineSaveTiming(
            "audio_mix_track",
            trackMixStartedAt,
            "track=\"" + jsonEscape(trackId) + "\" inputPackets=" + std::to_string(packets.size()) +
                " outputPackets=" + std::to_string(mixedTrack.size()) +
                " retainedSpan100ns=" + std::to_string(std::max<int64_t>(0, retainedEnd - retainedStart)));
        mixedAudioPacketCount += mixedTrack.size();
        normalizedAudioPackets.insert(normalizedAudioPackets.end(), mixedTrack.begin(), mixedTrack.end());
    }
    logEngineSaveTiming(
        "audio_mix",
        audioMixStartedAt,
        "tracks=" + std::to_string(audioByTrack.size()) +
            " outputPackets=" + std::to_string(mixedAudioPacketCount));

    std::vector<EncodedPacket> audioPackets;
    audioPackets.reserve(encodedAudioPackets.size() + normalizedAudioPackets.size());
    audioPackets.insert(
        audioPackets.end(),
        std::make_move_iterator(encodedAudioPackets.begin()),
        std::make_move_iterator(encodedAudioPackets.end()));
    audioPackets.insert(
        audioPackets.end(),
        std::make_move_iterator(normalizedAudioPackets.begin()),
        std::make_move_iterator(normalizedAudioPackets.end()));
    auto videoSegments = splitVideoSegmentRangesByEncodedResolution(clipPackets);
    std::vector<std::string> segmentFiles;
    std::vector<std::string> segmentResolutions;
    std::string outputFilePath;
    std::string muxMessage;
    const std::size_t selectedVideoPacketCount = clipPackets.size();
    std::vector<std::string> ioAnalysisEntries;
    auto rememberIoAnalysis = [&](const MuxResult& mux) {
        if (!mux.ioAnalysis.enabled) return;
        ioAnalysisEntries.push_back(saveIoAnalysisToJson(mux.ioAnalysis));
        std::ostringstream json;
        json << '[';
        for (std::size_t i = 0; i < ioAnalysisEntries.size(); ++i) {
            if (i > 0) json << ',';
            json << ioAnalysisEntries[i];
        }
        json << ']';
        result.ioAnalysisJson = json.str();
    };

    if (videoSegments.size() > 1) {
        for (const auto& segment : videoSegments) {
            if (segment.begin >= segment.end || segment.end > clipPackets.size()) continue;
            std::vector<EncodedPacket> segmentMuxPackets(
                clipPackets.begin() + static_cast<std::ptrdiff_t>(segment.begin),
                clipPackets.begin() + static_cast<std::ptrdiff_t>(segment.end));
            const auto& firstPacket = clipPackets[segment.begin];
            const auto& lastPacket = clipPackets[segment.end - 1];
            const int64_t segmentStart = firstPacket.pts100ns;
            const int64_t segmentEnd = lastPacket.pts100ns + std::max<int64_t>(lastPacket.duration100ns, 0);
            for (const auto& audioPacket : audioPackets) {
                const int64_t audioPacketEnd = audioPacket.pts100ns +
                    std::max<int64_t>(1, audioPacket.duration100ns);
                if (audioPacket.pts100ns < segmentEnd && audioPacketEnd > segmentStart) {
                    segmentMuxPackets.push_back(audioPacket);
                }
            }

            const int segmentWidth = segment.width > 0 ? segment.width : width;
            const int segmentHeight = segment.height > 0 ? segment.height : height;
            auto segmentPacing = savePacing;
            segmentPacing.presentationStartPts100ns = std::max(requestedClipStart, segmentStart);
            segmentPacing.presentationEndPts100ns = std::min(clipEnd, segmentEnd);
            const auto muxStartedAt = SaveTimingClock::now();
            const auto segmentMux = muxH264ToMp4(
                segmentMuxPackets,
                request.saveFolder,
                segmentWidth,
                segmentHeight,
                diagnostics_.fps,
                diagnostics_.bitrateMbps,
                std::move(segmentPacing));
            rememberIoAnalysis(segmentMux);
            logEngineSaveTiming(
                "mux_segment",
                muxStartedAt,
                "ok=" + std::string(segmentMux.ok ? "true" : "false") +
                    " packets=" + std::to_string(segmentMuxPackets.size()) +
                    " resolution=\"" + formatResolution(segmentWidth, segmentHeight) + "\"");
            if (!segmentMux.ok) {
                result.message = segmentMux.message;
                logEngineSaveTiming("total", totalStartedAt, saveTotalDetails("ok=false reason=mux_segment_failed"));
                return result;
            }
            segmentFiles.push_back(segmentMux.filePath);
            segmentResolutions.push_back(formatResolution(segmentWidth, segmentHeight));
            muxMessage = segmentMux.message;
        }
        if (segmentFiles.empty()) {
            result.message = "Segmented MP4 muxing failed: no video segments were writable.";
            logEngineSaveTiming("total", totalStartedAt, saveTotalDetails("ok=false reason=no_segment_files"));
            return result;
        }
        outputFilePath = stitchedPathForSegments(segmentFiles.front());
    } else {
        clipPackets.reserve(clipPackets.size() + audioPackets.size());
        clipPackets.insert(clipPackets.end(), audioPackets.begin(), audioPackets.end());
        const auto muxStartedAt = SaveTimingClock::now();
        const auto mux = muxH264ToMp4(
            clipPackets,
            request.saveFolder,
            width,
            height,
            diagnostics_.fps,
            diagnostics_.bitrateMbps,
            savePacing);
        rememberIoAnalysis(mux);
        logEngineSaveTiming(
            "mux",
            muxStartedAt,
            "ok=" + std::string(mux.ok ? "true" : "false") +
                " packets=" + std::to_string(clipPackets.size()) +
                " videoPackets=" + std::to_string(selectedVideoPacketCount) +
                " audioPackets=" + std::to_string(audioPackets.size()));

        if (!mux.ok) {
            result.message = mux.message;
            logEngineSaveTiming("total", totalStartedAt, saveTotalDetails("ok=false reason=mux_failed"));
            return result;
        }
        outputFilePath = mux.filePath;
        muxMessage = mux.message;
    }

    HMONITOR activeMonitor = captureSession_ ? static_cast<HMONITOR>(captureSession_->activeMonitor()) : nullptr;
    auto [fgApp, fgIsGame] = detectForegroundAppInfo(activeMonitor);
    std::vector<std::string> focusedApps;
    {
        std::lock_guard<std::mutex> lock(historyMutex_);
        auto now = std::chrono::steady_clock::now();
        auto cutoff = now - std::chrono::seconds(actualDuration);
        std::map<std::string, std::pair<int, bool>> appCounts;
        for (const auto& entry : foregroundHistory_) {
            if (entry.timestamp >= cutoff) {
                appCounts[entry.appName].first++;
                appCounts[entry.appName].second = entry.isGame;
            }
        }
        int maxAppCount = 0;
        int maxGameCount = 0;
        std::string topApp;
        std::string topGame;

        for (const auto& [appName, data] : appCounts) {
            if (appName != "Foreground app") {
                focusedApps.push_back(appName);
            }
            if (data.second) {
                if (data.first > maxGameCount) {
                    maxGameCount = data.first;
                    topGame = appName;
                }
            } else {
                if (data.first > maxAppCount) {
                    maxAppCount = data.first;
                    topApp = appName;
                }
            }
        }

        if (maxGameCount > 0) {
            fgApp = topGame;
            fgIsGame = true;
        } else if (maxAppCount > 0) {
            fgApp = topApp;
            fgIsGame = false;
        }
    }
    std::ostringstream clip;
    clip << "{"
         << "\"id\":\"clip-" << nowIdSuffix() << "\","
         << "\"title\":\"Clipture\","
         << "\"gameOrApp\":\"" << jsonEscape(fgApp) << "\","
         << "\"isGame\":" << (fgIsGame ? "true" : "false") << ","
         << "\"createdAt\":\"" << nowIsoLike() << "\","
         << "\"durationSeconds\":" << actualDuration << ","
         << "\"filePath\":\"" << jsonEscape(outputFilePath) << "\","
         << "\"resolution\":\"" << clipResolution << "\","
         << "\"recommendedResolution\":\"" << recommendedResolution << "\","
         << "\"fps\":" << diagnostics_.fps << ","
         << "\"encoder\":\"" << encoderName(diagnostics_.activeEncoder) << "\","
         << "\"audioTracks\":[";
    bool wroteTrack = false;
    for (const auto& packet : audioPackets) {
        if (packet.sourceId.empty()) continue;
        const auto trackName = packet.sourceId;
        const bool alreadyWritten = clip.str().find("\"" + trackName + "\"") != std::string::npos;
        if (alreadyWritten) continue;
        if (wroteTrack) clip << ",";
        clip << "\"" << jsonEscape(trackName) << "\"";
        wroteTrack = true;
    }
    clip << "]";
    if (!segmentFiles.empty()) {
        clip << ",\"segmentFiles\":[";
        for (std::size_t i = 0; i < segmentFiles.size(); ++i) {
            if (i > 0) clip << ",";
            clip << "\"" << jsonEscape(segmentFiles[i]) << "\"";
        }
        clip << "]";
        clip << ",\"segmentResolutions\":[";
        for (std::size_t i = 0; i < segmentResolutions.size(); ++i) {
            if (i > 0) clip << ",";
            clip << "\"" << jsonEscape(segmentResolutions[i]) << "\"";
        }
        clip << "]";
    }
    
    clip << ",\"focusedApps\":[";
    for (std::size_t i = 0; i < focusedApps.size(); ++i) {
        if (i > 0) clip << ",";
        clip << "\"" << jsonEscape(focusedApps[i]) << "\"";
    }
    clip << "]";
    
    clip << "}";

    result.ok = true;
    result.message = segmentFiles.empty()
        ? muxMessage
        : "Saved segmented MP4 clip for export stitching.";
    result.clipJson = clip.str();
    logEngineSaveTiming(
        "total",
        totalStartedAt,
        saveTotalDetails("ok=true videoPackets=" + std::to_string(selectedVideoPacketCount) +
            " audioPackets=" + std::to_string(audioPackets.size()) +
            " segments=" + std::to_string(segmentFiles.size())));
    return result;
}

void Engine::arm() {
    diagnostics_.bufferDurationSeconds = settings_.clipLengthSeconds;

    if (diagnostics_.activeEncoder != EncoderName::Nvenc) {
        diagnostics_.engineRunning = false;
        diagnostics_.d3d11Ready = false;
        diagnostics_.captureReady = false;
        diagnostics_.audioReady = false;
        diagnostics_.muxReady = false;
        diagnostics_.degraded = true;
        return;
    }

    const auto runtime = initializeNativeRuntime(true);
    diagnostics_.gpu = runtime.adapterName.empty() ? diagnostics_.gpu : runtime.adapterName;
    diagnostics_.resolution = runtime.resolution;
    diagnostics_.engineRunning = runtime.d3d11Ready;
    diagnostics_.d3d11Ready = runtime.d3d11Ready;

    if (videoReplayStore_) videoReplayStore_->start();
    if (aacReplayStore_) aacReplayStore_->start();
    if (pcmRecoveryStore_) pcmRecoveryStore_->start();
    encoderWorker_->start();
    {
        const auto [maxEncodeWidth, maxEncodeHeight] = encoderMaxDimensions(settings_);
        encoderWorker_->configure(
            settings_.fps,
            settings_.bitrateMbps,
            settings_.targetWidth,
            settings_.targetHeight,
            maxEncodeWidth,
            maxEncodeHeight,
            settings_.nvencPreset);
    }
    const bool captureStarted = captureSession_->startMonitor(&frameQueue_, &captureTickGate_, settings_.monitorId);
    diagnostics_.captureReady = captureStarted;
    if (captureStarted) {
        diagnostics_.resolution = captureSession_->resolution();
        diagnostics_.display = captureSession_->displayName();
        diagnostics_.hdrTonemapping = captureSession_->hdrTonemappingActive();
    }
    diagnostics_.audioReady = false;
    diagnostics_.muxReady = false;
    diagnostics_.degraded = !runtime.d3d11Ready || !captureStarted;
    diagnostics_.status = captureStarted ? captureSession_->status() : captureSession_->status();
    refreshAudioRouting();
    audioReplayCoordinator_->start();
    audioCaptureWorker_->start(false);
    diagnostics_.audioReady = audioCaptureWorker_->running();
    refreshPacketCounts();
}

void Engine::refreshPacketCounts() {
    maybeResetAutoVideoResolution();
    if (armed_ && captureSession_ && !captureSession_->running()) {
        const int64_t currentTime100ns = now100ns();
        if (currentTime100ns >= nextCaptureRestart100ns_) {
            nextCaptureRestart100ns_ = currentTime100ns + 2LL * 10'000'000LL;
            if (encoderWorker_) encoderWorker_->requireFreshFrame();
            const bool restarted = captureSession_->startMonitor(&frameQueue_, &captureTickGate_, settings_.monitorId);
            diagnostics_.captureReady = restarted;
            if (restarted) {
                nextCaptureRestart100ns_ = 0;
                diagnostics_.resolution = captureSession_->resolution();
                diagnostics_.display = captureSession_->displayName();
                diagnostics_.hdrTonemapping = captureSession_->hdrTonemappingActive();
            }
        }
    }
    diagnostics_.bufferedVideoPackets = static_cast<int>(
        videoReplayStore_ ? videoReplayStore_->size() : videoPackets_.size());
    diagnostics_.bufferedAudioPackets = static_cast<int>(audioPackets_.size());
    const auto videoArchive = videoReplayStore_ ? videoReplayStore_->stats() : ReplaySegmentStoreStats {};
    const auto audioArchive = aacReplayStore_ ? aacReplayStore_->stats() : ReplaySegmentStoreStats {};
    const auto pcmRecoveryArchive = pcmRecoveryStore_ ? pcmRecoveryStore_->stats() : ReplaySegmentStoreStats {};
    const auto audioReplay = audioReplayCoordinator_ ? audioReplayCoordinator_->stats() : AudioReplayStats {};
    diagnostics_.videoReplayArchiveHealthy = videoArchive.healthy;
    diagnostics_.audioReplayArchiveHealthy = audioArchive.healthy && pcmRecoveryArchive.healthy;
    diagnostics_.replayArchiveDiskBytes =
        videoArchive.diskBytes + audioArchive.diskBytes + pcmRecoveryArchive.diskBytes;
    diagnostics_.replayArchiveRamFallbackBytes =
        videoArchive.ramFallbackBytes + audioArchive.ramFallbackBytes + pcmRecoveryArchive.ramFallbackBytes;
    diagnostics_.replayArchiveResidentBytes =
        videoArchive.residentPayloadBytes + audioArchive.residentPayloadBytes +
        pcmRecoveryArchive.residentPayloadBytes;
    diagnostics_.replayArchiveResidentBudgetBytes =
        videoArchive.residentPayloadBudgetBytes + audioArchive.residentPayloadBudgetBytes +
        (audioReplay.pcmRecoveryActive ? pcmRecoveryArchive.residentPayloadBudgetBytes : 0);
    diagnostics_.replayArchiveReadCacheBytes =
        videoArchive.readCacheBytes + audioArchive.readCacheBytes + pcmRecoveryArchive.readCacheBytes;
    diagnostics_.replayArchiveResidentPackets = static_cast<int>(std::min<std::size_t>(
        videoArchive.residentPackets + audioArchive.residentPackets + pcmRecoveryArchive.residentPackets,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    diagnostics_.replayArchiveDiskBackedPackets = static_cast<int>(std::min<std::size_t>(
        videoArchive.diskBackedPackets + audioArchive.diskBackedPackets +
            pcmRecoveryArchive.diskBackedPackets,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    diagnostics_.replayArchiveQueuedBytes =
        videoArchive.queuedBytes + audioArchive.queuedBytes + pcmRecoveryArchive.queuedBytes;
    diagnostics_.replayArchivePersistedPackets =
        videoArchive.persistedPackets + audioArchive.persistedPackets + pcmRecoveryArchive.persistedPackets;
    diagnostics_.replayArchiveSpillCandidateInspections =
        videoArchive.spillCandidateInspections + audioArchive.spillCandidateInspections +
        pcmRecoveryArchive.spillCandidateInspections;
    diagnostics_.replayArchiveWriteFailures =
        videoArchive.writeFailures + audioArchive.writeFailures + pcmRecoveryArchive.writeFailures;
    diagnostics_.replayArchiveQueuedPackets = static_cast<int>(std::min<std::size_t>(
        videoArchive.queuedPackets + audioArchive.queuedPackets + pcmRecoveryArchive.queuedPackets,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    diagnostics_.replayArchiveSegments = static_cast<int>(std::min<std::size_t>(
        videoArchive.activeSegments + audioArchive.activeSegments + pcmRecoveryArchive.activeSegments,
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    diagnostics_.replayArchiveMaximumWriteBytes = static_cast<int>(std::min<std::size_t>(
        std::max({
            videoArchive.maximumWriteBytes,
            audioArchive.maximumWriteBytes,
            pcmRecoveryArchive.maximumWriteBytes
        }),
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
    diagnostics_.pcmRecoveryActive = audioReplay.pcmRecoveryActive;
    diagnostics_.queuedFrames = static_cast<int>(frameQueue_.size());
    diagnostics_.capturedFrames = captureSession_ ? captureSession_->capturedFrames() : 0;
    diagnostics_.encoderAcceptedFrames = encoderWorker_ ? encoderWorker_->framesAccepted() : 0;
    diagnostics_.encoderOutputPackets = encoderWorker_ ? encoderWorker_->framesEncoded() : 0;
    if (encoderWorker_) {
        const auto encoderStatus = encoderWorker_->status();
        if (!encoderStatus.empty()) {
            const auto captureStatus = captureSession_ ? captureSession_->status() : std::string("Capture status unavailable.");
            diagnostics_.status = "Capture: " + captureStatus + " Encoder: " + encoderStatus;
        }
        const int sourceWidth = encoderWorker_->sourceWidth();
        const int sourceHeight = encoderWorker_->sourceHeight();
        const int outputWidth = encoderWorker_->outputWidth();
        const int outputHeight = encoderWorker_->outputHeight();
        diagnostics_.videoSourceResolution = sourceWidth > 0 && sourceHeight > 0
            ? formatResolution(sourceWidth, sourceHeight)
            : diagnostics_.resolution;
        diagnostics_.videoOutputResolution = outputWidth > 0 && outputHeight > 0
            ? formatResolution(outputWidth, outputHeight)
            : "Waiting for first frame";
        diagnostics_.videoScaling = encoderWorker_->scalingActive() ? "GPU fit" : "None";
        if (pendingAutoVideoResolutionReset100ns_ > 0) {
            diagnostics_.videoScaling += " / relock pending";
        }
        diagnostics_.clipTargetResolution = settings_.targetWidth > 0 && settings_.targetHeight > 0
            ? formatResolution(settings_.targetWidth, settings_.targetHeight)
            : "Clip-aware system";
    }
    diagnostics_.audioCapturedPackets = audioCaptureWorker_ ? audioCaptureWorker_->packetsCaptured() : 0;
    if (audioCaptureWorker_) {
        diagnostics_.microphoneDevice = audioCaptureWorker_->microphoneStatus();
    }
    const auto captureStats = captureSession_
        ? captureSession_->runtimeStats()
        : CaptureRuntimeStats {};
    diagnostics_.captureApi = captureStats.activeBackend;
    diagnostics_.requestedCaptureBackend = captureStats.requestedBackend;
    diagnostics_.activeCaptureBackend = captureStats.activeBackend;
    diagnostics_.captureFallbackReason = captureStats.fallbackReason;
    diagnostics_.captureTargetKind = captureStats.targetKind;
    diagnostics_.captureTargetName = captureStats.targetName;
    diagnostics_.displayRefreshNumerator = captureStats.refreshNumerator;
    diagnostics_.displayRefreshDenominator = captureStats.refreshDenominator;
    diagnostics_.displayRefreshHz = captureStats.refreshHz;
    diagnostics_.captureAcquiredUpdates = captureStats.acquiredUpdates;
    diagnostics_.captureDesktopPresents = captureStats.desktopPresents;
    diagnostics_.capturePointerUpdates = captureStats.pointerUpdates;
    diagnostics_.capturePublishedFrames = captureStats.publishedFrames;
    diagnostics_.captureAccumulatedFrames = captureStats.accumulatedFrames;
    diagnostics_.captureAccumulationEvents = captureStats.accumulationEvents;
    diagnostics_.captureSamplerRejections = captureStats.samplerRejections;
    diagnostics_.captureNonMonotonicTimestamps = captureStats.nonMonotonicTimestamps;
    diagnostics_.captureAcquireTimeouts = captureStats.acquireTimeouts;
    diagnostics_.captureAcquireImmediateMisses = captureStats.acquireImmediateMisses;
    diagnostics_.captureAcquireGraceHits = captureStats.acquireGraceHits;
    diagnostics_.captureAcquireGraceTimeouts = captureStats.acquireGraceTimeouts;
    diagnostics_.captureAccessLosses = captureStats.accessLosses;
    diagnostics_.captureRecreationAttempts = captureStats.recreationAttempts;
    diagnostics_.captureRecreationSuccesses = captureStats.recreationSuccesses;
    diagnostics_.captureFallbacks = captureStats.fallbackCount;
    diagnostics_.captureClockMode = captureStats.clockMode;
    diagnostics_.captureClockTickRequests = captureStats.clockTickRequests;
    diagnostics_.captureClockTickWakeups = captureStats.clockTickWakeups;
    diagnostics_.captureClockTickCoalesced = captureStats.clockTickCoalesced;
    diagnostics_.captureClockTickCompletions = captureStats.clockTickCompletions;
    diagnostics_.captureClockTickCompletionWaits = captureStats.clockTickCompletionWaits;
    diagnostics_.captureClockTickCompletionTimeouts = captureStats.clockTickCompletionTimeouts;
    diagnostics_.presentLatchWaits = encoderWorker_ ? encoderWorker_->presentLatchWaits() : 0;
    diagnostics_.presentLatchHits = encoderWorker_ ? encoderWorker_->presentLatchHits() : 0;
    diagnostics_.presentLatchTimeouts = encoderWorker_ ? encoderWorker_->presentLatchTimeouts() : 0;
    diagnostics_.catchUpEvents = encoderWorker_ ? encoderWorker_->catchUpEvents() : 0;
    diagnostics_.historicalFramesRecovered = encoderWorker_
        ? encoderWorker_->historicalFramesRecovered()
        : 0;
    diagnostics_.catchUpRepeatedTicks = encoderWorker_ ? encoderWorker_->catchUpRepeatedTicks() : 0;
    diagnostics_.desktopPresentFps = captureStats.desktopPresentFps;
    diagnostics_.publishedFreshFps = captureStats.publishedFreshFps;
    diagnostics_.recentPublishedFreshFps = captureStats.recentPublishedFreshFps;
    diagnostics_.recentCaptureAcquireP95_100ns = captureStats.acquireWaitLatency.p95_100ns;
    diagnostics_.recentCaptureSourceIntervalP50_100ns = captureStats.sourceUpdateInterval.p50_100ns;
    diagnostics_.recentCaptureSourceIntervalP95_100ns = captureStats.sourceUpdateInterval.p95_100ns;
    diagnostics_.recentCaptureSourceIntervalMaximum100ns = captureStats.sourceUpdateInterval.maximum100ns;
    diagnostics_.recentPublishedPtsIntervalP50_100ns = captureStats.publishedPtsInterval.p50_100ns;
    diagnostics_.recentPublishedPtsIntervalP95_100ns = captureStats.publishedPtsInterval.p95_100ns;
    diagnostics_.recentPublishedPtsIntervalMaximum100ns = captureStats.publishedPtsInterval.maximum100ns;
    diagnostics_.recentPublishedWallIntervalP50_100ns = captureStats.publishedWallInterval.p50_100ns;
    diagnostics_.recentPublishedWallIntervalP95_100ns = captureStats.publishedWallInterval.p95_100ns;
    diagnostics_.recentPublishedWallIntervalMaximum100ns = captureStats.publishedWallInterval.maximum100ns;
    diagnostics_.recentCapturePreparationP50_100ns = captureStats.framePreparationLatency.p50_100ns;
    diagnostics_.recentCapturePreparationP95_100ns = captureStats.framePreparationLatency.p95_100ns;
    diagnostics_.recentCaptureCursorP95_100ns = captureStats.cursorCompositeLatency.p95_100ns;
    diagnostics_.recentCaptureProcessingP50_100ns = captureStats.frameProcessingLatency.p50_100ns;
    diagnostics_.recentCaptureProcessingP95_100ns = captureStats.frameProcessingLatency.p95_100ns;

    const auto queueStats = frameQueue_.stats();
    const auto boundedCounter = [](uint64_t value) {
        return static_cast<int>(std::min<uint64_t>(
            value,
            static_cast<uint64_t>(std::numeric_limits<int>::max())));
    };
    diagnostics_.captureOverflowDrops = boundedCounter(queueStats.overflowDrops);
    diagnostics_.captureCoalescedDrops = boundedCounter(queueStats.coalescedDrops);
    diagnostics_.frameQueueMaxDepth = queueStats.maximumDepth;
    diagnostics_.sourceFramesSuperseded = captureSession_
        ? boundedCounter(captureSession_->sourceFramesSuperseded())
        : 0;
    diagnostics_.captureSlotDrops = captureSession_ ? boundedCounter(captureSession_->ownedSlotDrops()) : 0;
    diagnostics_.captureCallbackErrors = captureSession_ ? boundedCounter(captureSession_->callbackErrors()) : 0;
    diagnostics_.maximumCaptureGap100ns = captureSession_ ? captureSession_->maximumFrameInterval100ns() : 0;
    diagnostics_.captureEpoch = captureSession_ ? captureSession_->captureEpoch() : 0;
    diagnostics_.schedulerDroppedFrames = encoderWorker_ ? encoderWorker_->schedulerDroppedFrames() : 0;
    diagnostics_.schedulerRepeatedFrames = encoderWorker_ ? encoderWorker_->schedulerRepeatedFrames() : 0;
    diagnostics_.stillFrameDuplicationEnabled = encoderWorker_
        ? encoderWorker_->stillFrameDuplicationEnabled()
        : false;
    diagnostics_.encoderDistinctSourceFrames = encoderWorker_
        ? encoderWorker_->distinctSourceOutputFrames()
        : 0;
    diagnostics_.encoderRepeatedSourceFrames = encoderWorker_
        ? encoderWorker_->repeatedSourceOutputFrames()
        : 0;
    diagnostics_.encoderUnknownSourceFrames = encoderWorker_
        ? encoderWorker_->unknownSourceOutputFrames()
        : 0;
    diagnostics_.motionFramesTotal = encoderWorker_
        ? encoderWorker_->motionFramesTotal()
        : 0;
    diagnostics_.motionFramesRepeated = encoderWorker_
        ? encoderWorker_->motionFramesRepeated()
        : 0;
    diagnostics_.encodedRepeatRatio = diagnostics_.encoderOutputPackets > 0
        ? static_cast<double>(diagnostics_.encoderRepeatedSourceFrames) /
            static_cast<double>(diagnostics_.encoderOutputPackets)
        : 0.0;
    diagnostics_.encodedMotionRepeatRatioPercent = diagnostics_.motionFramesTotal > 0
        ? (static_cast<double>(diagnostics_.motionFramesRepeated) * 100.0) /
            static_cast<double>(diagnostics_.motionFramesTotal)
        : 0.0;
    diagnostics_.encoderQueueDrops = encoderWorker_ ? encoderWorker_->encoderQueueDrops() : 0;
    diagnostics_.encoderRepeatCoalesced = encoderWorker_ ? encoderWorker_->encoderRepeatCoalesced() : 0;
    diagnostics_.encoderAdmissionRejections = encoderWorker_
        ? encoderWorker_->encoderAdmissionRejections()
        : 0;
    diagnostics_.encoderQueuedFreshFrames = encoderWorker_ ? encoderWorker_->queuedFreshEncodeFrames() : 0;
    diagnostics_.encoderQueuedRepeatFrames = encoderWorker_ ? encoderWorker_->queuedRepeatEncodeFrames() : 0;
    diagnostics_.nvencSurfaceDrops = encoderWorker_ ? encoderWorker_->nvencSurfaceDrops() : 0;
    diagnostics_.nvencInputDrops = encoderWorker_ ? encoderWorker_->nvencInputDrops() : 0;
    diagnostics_.encoderBackpressureDrops = encoderWorker_ ? encoderWorker_->encoderBackpressureDrops() : 0;
    diagnostics_.nvencInFlightFrames = encoderWorker_ ? encoderWorker_->nvencInFlightFrames() : 0;
    diagnostics_.maximumSubmitLatency100ns = encoderWorker_ ? encoderWorker_->maximumSubmitLatency100ns() : 0;
    diagnostics_.averageScaleLatency100ns = encoderWorker_ ? encoderWorker_->averageScaleLatency100ns() : 0;
    diagnostics_.maximumScaleLatency100ns = encoderWorker_ ? encoderWorker_->maximumScaleLatency100ns() : 0;
    diagnostics_.averageInputMapLatency100ns = encoderWorker_ ? encoderWorker_->averageInputMapLatency100ns() : 0;
    diagnostics_.maximumInputMapLatency100ns = encoderWorker_ ? encoderWorker_->maximumInputMapLatency100ns() : 0;
    diagnostics_.averageNvencCallLatency100ns = encoderWorker_ ? encoderWorker_->averageNvencCallLatency100ns() : 0;
    diagnostics_.maximumNvencCallLatency100ns = encoderWorker_ ? encoderWorker_->maximumNvencCallLatency100ns() : 0;
    diagnostics_.averageOutputDrainLatency100ns = encoderWorker_ ? encoderWorker_->averageOutputDrainLatency100ns() : 0;
    diagnostics_.maximumOutputDrainLatency100ns = encoderWorker_ ? encoderWorker_->maximumOutputDrainLatency100ns() : 0;
    const auto encoderRecent = encoderWorker_
        ? encoderWorker_->recentPerformance()
        : EncoderRecentPerformance {};
    diagnostics_.recentEncoderInputFps = encoderRecent.inputFps;
    diagnostics_.recentEncoderOutputFps = encoderRecent.outputFps;
    diagnostics_.recentEncoderDistinctSourceFps = encoderRecent.distinctSourceOutputFps;
    diagnostics_.recentEncoderRepeatedSourceFrames = encoderRecent.repeatedSourceOutputFrames;
    diagnostics_.recentEncoderUnknownSourceFrames = encoderRecent.unknownSourceOutputFrames;
    diagnostics_.recentMotionFramesTotal = encoderRecent.motionFramesTotal;
    diagnostics_.recentMotionFramesRepeated = encoderRecent.motionFramesRepeated;
    diagnostics_.recentMotionRepeatRatioPercent = encoderRecent.motionFramesTotal > 0
        ? (static_cast<double>(encoderRecent.motionFramesRepeated) * 100.0) /
            static_cast<double>(encoderRecent.motionFramesTotal)
        : 0.0;
    diagnostics_.recentSchedulerWakeLatenessP50_100ns = encoderRecent.schedulerWakeLateness.p50_100ns;
    diagnostics_.recentSchedulerWakeLatenessP95_100ns = encoderRecent.schedulerWakeLateness.p95_100ns;
    diagnostics_.recentSchedulerWakeLatenessMaximum100ns = encoderRecent.schedulerWakeLateness.maximum100ns;
    diagnostics_.recentEncoderQueueResidenceP50_100ns = encoderRecent.queueResidence.p50_100ns;
    diagnostics_.recentEncoderQueueResidenceP95_100ns = encoderRecent.queueResidence.p95_100ns;
    diagnostics_.recentEncoderQueueResidenceMaximum100ns = encoderRecent.queueResidence.maximum100ns;
    diagnostics_.recentEncoderInputIntervalP50_100ns = encoderRecent.inputPtsInterval.p50_100ns;
    diagnostics_.recentEncoderInputIntervalP95_100ns = encoderRecent.inputPtsInterval.p95_100ns;
    diagnostics_.recentEncoderInputIntervalMaximum100ns = encoderRecent.inputPtsInterval.maximum100ns;
    diagnostics_.recentEncoderOutputIntervalP50_100ns = encoderRecent.outputPtsInterval.p50_100ns;
    diagnostics_.recentEncoderOutputIntervalP95_100ns = encoderRecent.outputPtsInterval.p95_100ns;
    diagnostics_.recentEncoderOutputIntervalMaximum100ns = encoderRecent.outputPtsInterval.maximum100ns;
    diagnostics_.recentInputPreparationP50_100ns = encoderRecent.inputPreparation.p50_100ns;
    diagnostics_.recentInputPreparationP95_100ns = encoderRecent.inputPreparation.p95_100ns;
    diagnostics_.recentInputMapP50_100ns = encoderRecent.inputMap.p50_100ns;
    diagnostics_.recentInputMapP95_100ns = encoderRecent.inputMap.p95_100ns;
    diagnostics_.recentNvencCallP50_100ns = encoderRecent.encodeCall.p50_100ns;
    diagnostics_.recentNvencCallP95_100ns = encoderRecent.encodeCall.p95_100ns;
    diagnostics_.recentOutputEventWaitP50_100ns = encoderRecent.outputEventWait.p50_100ns;
    diagnostics_.recentOutputEventWaitP95_100ns = encoderRecent.outputEventWait.p95_100ns;
    diagnostics_.recentOutputLockP95_100ns = encoderRecent.outputLock.p95_100ns;
    diagnostics_.recentOutputCopyP95_100ns = encoderRecent.outputCopy.p95_100ns;
    diagnostics_.recentOutputUnmapP95_100ns = encoderRecent.outputUnmap.p95_100ns;
    diagnostics_.nvencZeroCopyFrames = encoderWorker_ ? encoderWorker_->nvencZeroCopyFrames() : 0;
    diagnostics_.nvencCopyFallbackFrames = encoderWorker_ ? encoderWorker_->nvencCopyFallbackFrames() : 0;
    diagnostics_.nvencConvertedFrames = encoderWorker_ ? encoderWorker_->nvencConvertedFrames() : 0;
    diagnostics_.droppedFrames = boundedCounter(
        queueStats.overflowDrops +
        static_cast<uint64_t>(std::max(0, diagnostics_.captureSlotDrops)) +
        static_cast<uint64_t>(std::max(0, diagnostics_.schedulerDroppedFrames)) +
        static_cast<uint64_t>(std::max(0, diagnostics_.encoderBackpressureDrops)));

    const int64_t frameInterval100ns = 10'000'000LL / std::max(1, diagnostics_.fps);
    const int64_t oldestFrameAge100ns = frameQueue_.oldestFrameAge100ns();
    if (diagnostics_.queuedFrames >= 4 ||
        diagnostics_.encoderQueuedFreshFrames >= 4 ||
        oldestFrameAge100ns > frameInterval100ns * 4) {
        diagnostics_.capturePressure = "critical";
    } else if (diagnostics_.queuedFrames >= 2 ||
               diagnostics_.encoderQueuedFreshFrames >= 2 ||
               oldestFrameAge100ns > frameInterval100ns * 2) {
        diagnostics_.capturePressure = "elevated";
    } else {
        diagnostics_.capturePressure = "healthy";
    }
}

void Engine::maybeResetAutoVideoResolution() {
    if (pendingAutoVideoResolutionReset100ns_ <= 0) return;
    pendingAutoVideoResolutionReset100ns_ = 0;
}

}  // namespace clipture
