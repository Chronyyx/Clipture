#include "clipture/Engine.hpp"
#include "clipture/CaptureSession.hpp"
#include "clipture/Mp4Muxer.hpp"
#include "clipture/NativeRuntime.hpp"

#include <Windows.h>

#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <regex>
#include <cstdlib>
#include <vector>
#include <thread>
#include <atomic>
#include <TlHelp32.h>

#include <map>
#include <mutex>
#include <set>

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
    FILETIME fileTime {};
    GetSystemTimePreciseAsFileTime(&fileTime);
    ULARGE_INTEGER value {};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return static_cast<int64_t>(value.QuadPart);
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

std::string stripExtension(std::string value);

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
    return stripExtension(processName);
}

std::string captureProcessName(const std::string& sourceSpec) {
    if (sourceSpec.rfind("game:", 0) == 0) return sourceSpec.substr(5);
    if (sourceSpec.rfind("app:", 0) == 0) return sourceSpec.substr(4);
    return sourceSpec;
}

bool containsProcessName(const std::vector<std::string>& sourceSpecs, const std::string& processName) {
    const auto wanted = lowerAscii(captureProcessName(processName));
    if (wanted.empty()) return true;
    return std::any_of(sourceSpecs.begin(), sourceSpecs.end(), [&](const std::string& sourceSpec) {
        return lowerAscii(captureProcessName(sourceSpec)) == wanted;
    });
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
    if (gameName != stripExtension(exeName)) {
        std::lock_guard<std::mutex> lock(g_exeToGameNameMutex);
        g_exeToGameName[lowerAscii(exeName)] = gameName;
        return true;
    }
    return false;
}

bool isProcessRunningByName(const std::string& exeName) {
    const auto wanted = lowerAscii(captureProcessName(exeName));
    if (wanted.empty()) return false;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (lowerAscii(narrowWide(entry.szExeFile)) == wanted) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
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

std::vector<std::vector<EncodedPacket>> splitVideoSegmentsByEncodedResolution(const std::vector<EncodedPacket>& packets) {
    std::vector<std::vector<EncodedPacket>> segments;
    int currentWidth = 0;
    int currentHeight = 0;
    for (const auto& packet : packets) {
        if (packet.kind != PacketKind::Video || packet.payload.empty()) continue;
        const int packetWidth = packet.encodedWidth > 0 ? packet.encodedWidth : currentWidth;
        const int packetHeight = packet.encodedHeight > 0 ? packet.encodedHeight : currentHeight;
        if (segments.empty() ||
            (packetWidth > 0 && packetHeight > 0 && (packetWidth != currentWidth || packetHeight != currentHeight))) {
            segments.push_back({});
            currentWidth = packetWidth;
            currentHeight = packetHeight;
        }
        segments.back().push_back(packet);
    }
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

bool startCodeAt(const std::vector<std::byte>& data, std::size_t offset, std::size_t& size) {
    if (offset + 3 <= data.size() &&
        data[offset] == std::byte{0} &&
        data[offset + 1] == std::byte{0} &&
        data[offset + 2] == std::byte{1}) {
        size = 3;
        return true;
    }
    if (offset + 4 <= data.size() &&
        data[offset] == std::byte{0} &&
        data[offset + 1] == std::byte{0} &&
        data[offset + 2] == std::byte{0} &&
        data[offset + 3] == std::byte{1}) {
        size = 4;
        return true;
    }
    return false;
}

bool packetContainsIdrFrame(const EncodedPacket& packet) {
    const auto& data = packet.payload;
    for (std::size_t i = 0; i + 4 < data.size(); ++i) {
        std::size_t startCodeSize = 0;
        if (!startCodeAt(data, i, startCodeSize)) continue;
        const std::size_t nalOffset = i + startCodeSize;
        if (nalOffset >= data.size()) continue;
        const auto nalType = std::to_integer<unsigned char>(data[nalOffset]) & 0x1F;
        if (nalType == 5) return true;
    }
    return false;
}

bool isKeyframePacket(const EncodedPacket& packet) {
    return packet.keyframe || packetContainsIdrFrame(packet);
}

int actualClipDurationSeconds(const std::vector<EncodedPacket>& packets, int requestedDurationSeconds) {
    if (packets.empty()) return requestedDurationSeconds;
    const int64_t firstPts = packets.front().pts100ns;
    const int64_t lastPts = packets.back().pts100ns;
    const int64_t lastDuration = packets.back().duration100ns > 0 ? packets.back().duration100ns : 0;
    const int64_t span = std::max<int64_t>(0, lastPts - firstPts + lastDuration);
    return std::clamp(static_cast<int>((span + 5'000'000LL) / 10'000'000LL), 1, requestedDurationSeconds);
}

std::vector<EncodedPacket> selectVideoWindowForClip(const std::vector<EncodedPacket>& packets, int durationSeconds) {
    std::vector<EncodedPacket> video;
    for (const auto& packet : packets) {
        if (packet.kind == PacketKind::Video && !packet.payload.empty()) {
            auto copy = packet;
            copy.keyframe = isKeyframePacket(packet);
            video.push_back(std::move(copy));
        }
    }
    if (video.empty()) return {};

    std::sort(video.begin(), video.end(), [](const EncodedPacket& a, const EncodedPacket& b) {
        return a.pts100ns < b.pts100ns;
    });

    const int64_t newestPts = video.back().pts100ns;
    const int64_t requestedStart = newestPts - (static_cast<int64_t>(durationSeconds) * 10'000'000LL);

    std::size_t firstIndex = video.size();
    for (std::size_t i = 0; i < video.size(); ++i) {
        if (video[i].pts100ns >= requestedStart) {
            firstIndex = i;
            break;
        }
    }
    if (firstIndex == video.size()) return {};

    for (std::size_t i = firstIndex + 1; i > 0; --i) {
        const std::size_t index = i - 1;
        if (isKeyframePacket(video[index])) {
            firstIndex = index;
            break;
        }
    }

    if (!isKeyframePacket(video[firstIndex])) {
        for (std::size_t i = firstIndex; i < video.size(); ++i) {
            if (isKeyframePacket(video[i])) {
                firstIndex = i;
                break;
            }
        }
    }

    if (!isKeyframePacket(video[firstIndex])) firstIndex = 0;

    return { video.begin() + static_cast<std::ptrdiff_t>(firstIndex), video.end() };
}

}  // namespace

Engine::Engine()
    : diagnostics_(collectDiagnostics()),
      frameQueue_(8),
      videoPackets_(5LL * 60LL * 10'000'000LL),
      audioPackets_(5LL * 60LL * 10'000'000LL),
      captureSession_(std::make_unique<CaptureSession>()),
      encoderWorker_(std::make_unique<EncoderWorker>(frameQueue_, videoPackets_)),
      audioCaptureWorker_(std::make_unique<AudioCaptureWorker>(audioPackets_)) {
    gameDetectionRunning_ = true;
    gameDetectionThread_ = std::thread([this] { gameDetectionLoop(); });
    arm();
}

Engine::~Engine() {
    gameDetectionRunning_ = false;
    if (gameDetectionThread_.joinable()) {
        gameDetectionThread_.join();
    }
}

void Engine::gameDetectionLoop() {
    while (gameDetectionRunning_) {
        {
            HMONITOR activeMonitor = captureSession_ ? static_cast<HMONITOR>(captureSession_->activeMonitor()) : nullptr;
            auto [fgApp, fgIsGame] = detectForegroundAppInfo(activeMonitor);
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
            std::vector<std::string> dynamicSources;
            std::vector<std::string> detectedGames;
            std::vector<std::string> foregroundSystemApps;
            
            HWND foreground = GetForegroundWindow();
            if (foreground) {
                DWORD fgPid = 0;
                GetWindowThreadProcessId(foreground, &fgPid);
                if (fgPid > 0) {
                    HANDLE fgProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fgPid);
                    if (fgProcess) {
                        wchar_t path[MAX_PATH] {};
                        DWORD size = MAX_PATH;
                        if (QueryFullProcessImageNameW(fgProcess, 0, path, &size) && size > 0) {
                            std::string narrowPath = narrowWide(path);
                            auto slashPos = narrowPath.find_last_of("\\/");
                            std::string fgExeName = (slashPos != std::string::npos) ? narrowPath.substr(slashPos + 1) : narrowPath;

                            if (!isIgnoredForegroundProcess(fgExeName)) {
                                const bool isGame = isKnownGameProcess(fgExeName, narrowPath, fgPid, foreground);
                                if (settings_.captureGameAudio && isGame &&
                                    !containsProcessName(settings_.appAudioProcesses, fgExeName) &&
                                    !containsProcessName(dynamicSources, fgExeName)) {
                                    dynamicSources.push_back("game:" + fgExeName);
                                    detectedGames.push_back(fgExeName);
                                } else if (settings_.captureForegroundSystemAudio && !isGame &&
                                    !containsProcessName(settings_.appAudioProcesses, fgExeName)) {
                                    foregroundSystemProcesses_.erase(
                                        std::remove_if(
                                            foregroundSystemProcesses_.begin(),
                                            foregroundSystemProcesses_.end(),
                                            [&](const std::string& name) {
                                                return lowerAscii(captureProcessName(name)) == lowerAscii(fgExeName);
                                            }),
                                        foregroundSystemProcesses_.end());
                                    foregroundSystemProcesses_.insert(foregroundSystemProcesses_.begin(), fgExeName);
                                    if (foregroundSystemProcesses_.size() > 3) {
                                        foregroundSystemProcesses_.resize(3);
                                    }
                                    foregroundSystemApps.push_back(fgExeName);
                                }
                            }
                        }
                        CloseHandle(fgProcess);
                    }
                }
            }
            
            if (settings_.captureGameAudio) {
                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snapshot != INVALID_HANDLE_VALUE) {
                    PROCESSENTRY32W entry {};
                    entry.dwSize = sizeof(entry);
                    if (Process32FirstW(snapshot, &entry)) {
                        do {
                            std::string exeName = narrowWide(entry.szExeFile);
                            if (containsProcessName(settings_.appAudioProcesses, exeName) ||
                                containsProcessName(dynamicSources, exeName)) {
                                continue;
                            }

                            bool isGame = false;
                            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                            if (process) {
                                wchar_t path[MAX_PATH] {};
                                DWORD size = MAX_PATH;
                                if (QueryFullProcessImageNameW(process, 0, path, &size) && size > 0) {
                                    isGame = isKnownGameProcess(exeName, narrowWide(path), entry.th32ProcessID);
                                }
                                CloseHandle(process);
                            }

                            if (isGame) {
                                dynamicSources.push_back("game:" + exeName);
                                detectedGames.push_back(exeName);
                            }
                        } while (Process32NextW(snapshot, &entry));
                    }
                    CloseHandle(snapshot);
                }
            }

            if (settings_.captureForegroundSystemAudio) {
                foregroundSystemProcesses_.erase(
                    std::remove_if(
                        foregroundSystemProcesses_.begin(),
                        foregroundSystemProcesses_.end(),
                        [&](const std::string& name) {
                            return isIgnoredForegroundProcess(name) ||
                                containsProcessName(settings_.appAudioProcesses, name) ||
                                !isProcessRunningByName(name);
                        }),
                    foregroundSystemProcesses_.end());

                if (foregroundSystemProcesses_.size() > 3) {
                    foregroundSystemProcesses_.resize(3);
                }

                for (const auto& processName : foregroundSystemProcesses_) {
                    if (!containsProcessName(dynamicSources, processName)) {
                        dynamicSources.push_back(processName);
                    }
                }
            }
            
            std::vector<std::string> mergedSources = settings_.appAudioProcesses;
            for (const auto& sourceSpec : dynamicSources) {
                if (!containsProcessName(mergedSources, sourceSpec)) {
                    mergedSources.push_back(sourceSpec);
                }
            }
            if (!detectedGames.empty()) {
                std::cerr << "[engine] Detected games in loop:";
                for (const auto& game : detectedGames) std::cerr << " " << game;
                std::cerr << std::endl;
            }
            if (!foregroundSystemApps.empty()) {
                std::cerr << "[engine] Foreground app added to system-specific audio:";
                for (const auto& app : foregroundSystemApps) std::cerr << " " << app;
                std::cerr << std::endl;
            }
            audioCaptureWorker_->configureAppSources(mergedSources);
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

const Diagnostics& Engine::diagnostics() {
    refreshPacketCounts();
    return diagnostics_;
}

const Diagnostics& Engine::configure(const EngineSettings& settings) {
    const auto previousMonitorId = settings_.monitorId;
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

    settings_.fps = std::clamp(settings.fps, 24, 60);
    settings_.bitrateMbps = std::clamp(settings.bitrateMbps, 4, 120);
    settings_.nvencPreset = std::clamp(settings.nvencPreset, 1, 5);
    settings_.clipLengthSeconds = std::clamp(settings.clipLengthSeconds, 5, 600);
    settings_.monitorId = settings.monitorId.empty() ? "primary" : settings.monitorId;
    settings_.targetWidth = std::max(0, settings.targetWidth);
    settings_.targetHeight = std::max(0, settings.targetHeight);
    if (settings_.targetWidth > 0 && settings_.targetHeight > 0) {
        pendingAutoVideoResolutionReset100ns_ = 0;
    }
    settings_.includeMixedAudio = settings.includeMixedAudio;
    settings_.includeSystemAudio = settings.includeSystemAudio;
    settings_.includeMicrophoneAudio = settings.includeMicrophoneAudio;
    settings_.captureGameAudio = settings.captureGameAudio;
    settings_.captureForegroundSystemAudio = settings.captureForegroundSystemAudio;
    if (!settings_.captureForegroundSystemAudio) {
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
    if (encoderWorker_) {
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
    if (captureSession_) {
        captureSession_->setTargetFps(settings_.fps);
        if (captureSession_->running() && previousMonitorId != settings_.monitorId) {
            if (encoderWorker_) {
                if (settings_.targetWidth <= 0 || settings_.targetHeight <= 0) {
                    encoderWorker_->resetAutoOutputResolution();
                } else {
                    encoderWorker_->requireFreshFrame();
                }
            }
            const bool captureStarted = captureSession_->startMonitor(&frameQueue_, settings_.monitorId);
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
        previousAppAudioProcesses != settings_.appAudioProcesses;
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
        audioCaptureWorker_->configureAppSources(settings_.appAudioProcesses);
    }
    if (audioCaptureWorker_ && micSettingsChanged) {
        audioCaptureWorker_->setMicSettings(settings_.micVolume, settings_.micIsolation, settings_.micIsolationWeight, settings_.noiseGateEnabled, settings_.autoNoiseGate, settings_.noiseGateThreshold, settings_.noiseGateDebounceMs, settings_.micDeviceId, settings_.micDeviceMatchKey, settings_.micDeviceName);
    }

    diagnostics_.fps = settings_.fps;
    diagnostics_.bitrateMbps = settings_.bitrateMbps;
    diagnostics_.bufferDurationSeconds = settings_.clipLengthSeconds;
    diagnostics_.encoderMode = "NVENC async P" + std::to_string(settings_.nvencPreset) + " (4 output buffers)";
    diagnostics_.microphoneDevice = audioCaptureWorker_ ? audioCaptureWorker_->microphoneStatus() : microphoneDeviceName(settings_.micDeviceId);
    diagnostics_.clipTargetResolution = settings_.targetWidth > 0 && settings_.targetHeight > 0
        ? formatResolution(settings_.targetWidth, settings_.targetHeight)
        : "Clip-aware system";

    const auto retention100ns = static_cast<int64_t>(settings_.clipLengthSeconds + 5) * 10'000'000LL;
    videoPackets_.setRetention(retention100ns);
    audioPackets_.setRetention(retention100ns);
    refreshPacketCounts();
    return diagnostics_;
}

std::vector<EncodedPacket> mixPcmPackets(const std::vector<EncodedPacket>& packets, const std::string& newSourceId, int64_t clipStart, int64_t clipEnd) {
    if (packets.empty()) return {};
    
    int sampleRate = 0;
    int channels = 0;
    
    for (const auto& p : packets) {
        if (sampleRate == 0) sampleRate = p.sampleRate;
        if (channels == 0) channels = p.channelCount;
    }
    
    if (clipStart >= clipEnd || sampleRate == 0 || channels == 0) return {};
    
    int64_t totalDuration = clipEnd - clipStart;
    int64_t totalSamples = (totalDuration * sampleRate) / 10'000'000;
    std::vector<float> mixBuffer(totalSamples * channels, 0.0f);
    
    for (const auto& p : packets) {
        if (p.sampleRate != sampleRate || p.channelCount != channels) continue;
        int64_t offset100ns = p.pts100ns - clipStart;
        if (offset100ns < 0) continue;
        int64_t offsetSamples = (offset100ns * sampleRate) / 10'000'000;
        
        const int16_t* pcm = reinterpret_cast<const int16_t*>(p.payload.data());
        int numSamples = static_cast<int>(p.payload.size() / (channels * sizeof(int16_t)));
        
        for (int i = 0; i < numSamples; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                int64_t mixIdx = (offsetSamples + i) * channels + ch;
                if (mixIdx >= 0 && mixIdx < static_cast<int64_t>(mixBuffer.size())) {
                    mixBuffer[mixIdx] += pcm[i * channels + ch];
                }
            }
        }
    }
    bool hasAudio = false;
    for (const auto& val : mixBuffer) {
        if (std::abs(val) >= 1.0f) {
            hasAudio = true;
            break;
        }
    }
    
    if (!hasAudio) {
        return {};
    }
    
    std::vector<EncodedPacket> mixed;
    int framesPerPacket = sampleRate / 100;
    
    for (int64_t i = 0; i < totalSamples; i += framesPerPacket) {
        int frames = static_cast<int>(std::min<int64_t>(framesPerPacket, totalSamples - i));
        EncodedPacket mp;
        mp.kind = PacketKind::Audio;
        mp.pts100ns = clipStart + (i * 10'000'000) / sampleRate;
        mp.dts100ns = mp.pts100ns;
        mp.duration100ns = (frames * 10'000'000) / sampleRate;
        mp.sourceId = newSourceId;
        mp.encoderId = "PCM_S16";
        mp.sampleRate = sampleRate;
        mp.channelCount = channels;
        mp.bitsPerSample = 16;
        mp.payload.resize(frames * channels * sizeof(int16_t));
        
        int16_t* outPcm = reinterpret_cast<int16_t*>(mp.payload.data());
        for (int j = 0; j < frames; ++j) {
            for (int ch = 0; ch < channels; ++ch) {
                float val = mixBuffer[(i + j) * channels + ch];
                outPcm[j * channels + ch] = static_cast<int16_t>(std::clamp(val, -32768.0f, 32767.0f));
            }
        }
        mixed.push_back(std::move(mp));
    }
    return mixed;
}

SaveClipResult Engine::saveClip(const SaveClipRequest& request) {
    SaveClipResult result;

    if (diagnostics_.activeEncoder != EncoderName::Nvenc) {
        result.message = "Cannot save clip: direct NVENC is not available, and this MVP requires NVENC first.";
        return result;
    }

    if (videoPackets_.size() == 0) {
        result.message = "No encoded H.264 packets are buffered yet. Keep diagnostics open and wait for Encoder packets to rise above 0.";
        return result;
    }

    const int duration = std::clamp(request.durationSeconds, 5, 600);
    auto clipPackets = selectVideoWindowForClip(videoPackets_.snapshot(), duration);
    if (clipPackets.empty()) {
        result.message = "No complete keyframe-starting H.264 window is buffered yet. Wait about one second and try again.";
        return result;
    }
    const int actualDuration = actualClipDurationSeconds(clipPackets, duration);

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
    std::vector<EncodedPacket> muxPackets = clipPackets;
    const int64_t clipStart = clipPackets.front().pts100ns;
    const int64_t clipEnd = clipPackets.back().pts100ns + std::max<int64_t>(clipPackets.back().duration100ns, 0);
    auto capturedAudioPackets = audioPackets_.selectWindow(clipStart, clipEnd);
    std::set<std::string> runningAppSources;
    for (const auto& app : settings_.appAudioProcesses) {
        if (isProcessRunningByName(captureProcessName(app))) {
            runningAppSources.insert("app:" + app);
        }
    }

    std::map<std::string, std::vector<EncodedPacket>> audioByTrack;
    for (const auto& packet : capturedAudioPackets) {
        if (packet.sourceId == "system-loopback-pcm" && settings_.includeSystemAudio) {
            audioByTrack["system-loopback-pcm"].push_back(packet);
        } else if (packet.sourceId == "microphone-pcm" && settings_.includeMicrophoneAudio) {
            audioByTrack["microphone-pcm"].push_back(packet);
        } else if (packet.sourceId.rfind("game:", 0) == 0) {
            std::string exeName = packet.sourceId.substr(5);
            std::string gameName = getGameName(exeName);
            if (isMinimizedRobloxName(gameName)) continue;
            if (isProcessRunningByName(captureProcessName(exeName))) {
                audioByTrack["game:" + gameName].push_back(packet);
            }
        } else if (packet.sourceId.rfind("app:", 0) == 0) {
            if (runningAppSources.find(packet.sourceId) != runningAppSources.end()) {
                audioByTrack[packet.sourceId].push_back(packet);
            }
        }
    }
    
    std::vector<EncodedPacket> normalizedAudioPackets;
    for (const auto& [trackId, packets] : audioByTrack) {
        auto mixedTrack = mixPcmPackets(packets, trackId, clipStart, clipEnd);
        normalizedAudioPackets.insert(normalizedAudioPackets.end(), mixedTrack.begin(), mixedTrack.end());
    }

    std::vector<EncodedPacket> audioPackets = normalizedAudioPackets;
    auto videoSegments = splitVideoSegmentsByEncodedResolution(clipPackets);
    std::vector<std::string> segmentFiles;
    std::vector<std::string> segmentResolutions;
    std::string outputFilePath;
    std::string muxMessage;

    if (videoSegments.size() > 1) {
        for (const auto& segment : videoSegments) {
            if (segment.empty()) continue;
            std::vector<EncodedPacket> segmentMuxPackets = segment;
            const int64_t segmentStart = segment.front().pts100ns;
            const int64_t segmentEnd = segment.back().pts100ns + std::max<int64_t>(segment.back().duration100ns, 0);
            for (const auto& [trackId, trackPackets] : audioByTrack) {
                auto mixedTrack = mixPcmPackets(trackPackets, trackId, segmentStart, segmentEnd);
                segmentMuxPackets.insert(segmentMuxPackets.end(), mixedTrack.begin(), mixedTrack.end());
            }

            auto [segmentWidth, segmentHeight] = encodedResolutionFromPackets(segment);
            if (segmentWidth <= 0 || segmentHeight <= 0) {
                segmentWidth = width;
                segmentHeight = height;
            }
            const auto segmentMux = muxH264ToMp4(
                segmentMuxPackets,
                request.saveFolder,
                segmentWidth,
                segmentHeight,
                diagnostics_.fps,
                diagnostics_.bitrateMbps);
            if (!segmentMux.ok) {
                result.message = segmentMux.message;
                return result;
            }
            segmentFiles.push_back(segmentMux.filePath);
            segmentResolutions.push_back(formatResolution(segmentWidth, segmentHeight));
            muxMessage = segmentMux.message;
        }
        if (segmentFiles.empty()) {
            result.message = "Segmented MP4 muxing failed: no video segments were writable.";
            return result;
        }
        outputFilePath = stitchedPathForSegments(segmentFiles.front());
    } else {
        muxPackets.insert(muxPackets.end(), audioPackets.begin(), audioPackets.end());
        const auto mux = muxH264ToMp4(
            muxPackets,
            request.saveFolder,
            width,
            height,
            diagnostics_.fps,
            diagnostics_.bitrateMbps);

        if (!mux.ok) {
            result.message = mux.message;
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
    captureSession_->setTargetFps(settings_.fps);
    const bool captureStarted = captureSession_->startMonitor(&frameQueue_, settings_.monitorId);
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
    audioCaptureWorker_->start();
    diagnostics_.audioReady = audioCaptureWorker_->running();
    refreshPacketCounts();
}

void Engine::refreshPacketCounts() {
    maybeResetAutoVideoResolution();
    diagnostics_.bufferedVideoPackets = static_cast<int>(videoPackets_.size());
    diagnostics_.bufferedAudioPackets = static_cast<int>(audioPackets_.size());
    diagnostics_.queuedFrames = static_cast<int>(frameQueue_.size());
    diagnostics_.capturedFrames = captureSession_ ? captureSession_->capturedFrames() : 0;
    diagnostics_.encoderAcceptedFrames = encoderWorker_ ? encoderWorker_->framesAccepted() : 0;
    diagnostics_.encoderOutputPackets = encoderWorker_ ? encoderWorker_->framesEncoded() : 0;
    if (encoderWorker_) {
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
    diagnostics_.droppedFrames = frameQueue_.droppedFrames();
}

void Engine::maybeResetAutoVideoResolution() {
    if (pendingAutoVideoResolutionReset100ns_ <= 0) return;
    pendingAutoVideoResolutionReset100ns_ = 0;
}

}  // namespace clipture
