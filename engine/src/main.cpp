#include "clipture/Engine.hpp"

#include <Windows.h>
#include <mmsystem.h>

#include "clipture/RawInputHotkey.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::mutex outputMutex;

int extractId(const std::string& line) {
    const auto marker = line.find("\"id\"");
    if (marker == std::string::npos) return 0;
    const auto colon = line.find(':', marker);
    if (colon == std::string::npos) return 0;
    return std::stoi(line.substr(colon + 1));
}

int extractDuration(const std::string& line) {
    const auto marker = line.find("\"durationSeconds\"");
    if (marker == std::string::npos) return 30;
    const auto colon = line.find(':', marker);
    if (colon == std::string::npos) return 30;
    return std::stoi(line.substr(colon + 1));
}

int extractInt(const std::string& line, const std::string& field, int fallback) {
    const auto marker = line.find("\"" + field + "\"");
    if (marker == std::string::npos) return fallback;
    const auto colon = line.find(':', marker);
    if (colon == std::string::npos) return fallback;
    return std::stoi(line.substr(colon + 1));
}

bool extractBool(const std::string& line, const std::string& field, bool fallback) {
    const auto marker = line.find("\"" + field + "\"");
    if (marker == std::string::npos) return fallback;
    const auto colon = line.find(':', marker);
    if (colon == std::string::npos) return fallback;
    const auto valueStart = line.find_first_not_of(" \t\r\n", colon + 1);
    if (valueStart == std::string::npos) return fallback;
    if (line.compare(valueStart, 4, "true") == 0) return true;
    if (line.compare(valueStart, 5, "false") == 0) return false;
    return fallback;
}

std::string extractString(const std::string& line, const std::string& field) {
    const auto marker = line.find("\"" + field + "\"");
    if (marker == std::string::npos) return {};
    const auto colon = line.find(':', marker);
    if (colon == std::string::npos) return {};
    const auto firstQuote = line.find('"', colon + 1);
    if (firstQuote == std::string::npos) return {};
    std::string result;
    bool escaped = false;
    for (auto i = firstQuote + 1; i < line.size(); ++i) {
        const char ch = line[i];
        if (escaped) {
            switch (ch) {
                case '\\': result.push_back('\\'); break;
                case '"': result.push_back('"'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default: result.push_back(ch); break;
            }
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') break;
        result.push_back(ch);
    }
    return result;
}

float extractFloat(const std::string& line, const std::string& field, float fallback) {
    const auto marker = line.find("\"" + field + "\"");
    if (marker == std::string::npos) return fallback;
    const auto colon = line.find(':', marker);
    if (colon == std::string::npos) return fallback;
    const auto valueStart = line.find_first_not_of(" \t\r\n", colon + 1);
    if (valueStart == std::string::npos) return fallback;
    try {
        return std::stof(line.substr(valueStart));
    } catch (...) {
        return fallback;
    }
}

std::vector<std::string> splitList(const std::string& value) {
    std::vector<std::string> values;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, '|')) {
        if (!item.empty()) values.push_back(item);
    }
    return values;
}

void respond(int id, const std::string& payload) {
    std::lock_guard lock(outputMutex);
    std::cout << "{\"id\":" << id << ",\"payload\":" << payload << "}" << std::endl;
}

void respondError(int id, const std::string& error) {
    std::lock_guard lock(outputMutex);
    std::cout << "{\"id\":" << id << ",\"error\":\"" << error << "\"}" << std::endl;
}

std::string jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

void emitHotkeyEvent() {
    std::lock_guard lock(outputMutex);
    std::cout << "{\"event\":\"hotkey\",\"source\":\"raw-input\"}" << std::endl;
}

class SavePriorityGuard {
public:
    SavePriorityGuard()
        : thread_(GetCurrentThread()),
          previousPriority_(GetThreadPriority(thread_)) {
        belowNormalSet_ = SetThreadPriority(thread_, THREAD_PRIORITY_BELOW_NORMAL) != 0;
        std::cerr << "[save-timing] source=engine stage=save_priority"
                  << " previousPriority=" << previousPriority_
                  << " backgroundBegin=false"
                  << " adaptiveBackground=true"
                  << " belowNormal=" << (belowNormalSet_ ? "true" : "false")
                  << std::endl;
    }

    ~SavePriorityGuard() {
        bool restoredPriority = false;
        if (previousPriority_ != THREAD_PRIORITY_ERROR_RETURN) {
            restoredPriority = SetThreadPriority(thread_, previousPriority_) != 0;
        }
        std::cerr << "[save-timing] source=engine stage=save_priority_restore"
                  << " backgroundEnd=false"
                  << " restoredPriority=" << (restoredPriority ? "true" : "false")
                  << std::endl;
    }

    SavePriorityGuard(const SavePriorityGuard&) = delete;
    SavePriorityGuard& operator=(const SavePriorityGuard&) = delete;

private:
    HANDLE thread_ = nullptr;
    int previousPriority_ = THREAD_PRIORITY_ERROR_RETURN;
    bool belowNormalSet_ = false;
};

}  // namespace

int main() {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }

    clipture::RawInputHotkey hotkey(emitHotkeyEvent);
    clipture::Engine engine;
    std::string line;

    while (std::getline(std::cin, line)) {
        const int id = extractId(line);
        try {
            if (line.find("\"getDiagnostics\"") != std::string::npos) {
                respond(id, clipture::toJson(engine.diagnostics()));
                continue;
            }

            if (line.find("\"configureHotkey\"") != std::string::npos) {
                const bool armed = hotkey.configure(extractString(line, "hotkey"));
                std::ostringstream payload;
                payload << "{\"ready\":" << (hotkey.ready() ? "true" : "false")
                        << ",\"armed\":" << (armed ? "true" : "false")
                        << ",\"status\":\"" << jsonEscape(hotkey.status()) << "\"}";
                respond(id, payload.str());
                continue;
            }

            if (line.find("\"listAudioInputDevices\"") != std::string::npos) {
                respond(id, clipture::audioInputDevicesJson());
                continue;
            }

            if (line.find("\"listDisplayDevices\"") != std::string::npos) {
                respond(id, clipture::displayDevicesJson());
                continue;
            }

            if (line.find("\"listRunningProcesses\"") != std::string::npos) {
                respond(id, engine.runningProcessesJson(extractBool(line, "includeExecutablePaths", false)));
                continue;
            }

            if (line.find("\"getProcessExecutablePath\"") != std::string::npos) {
                respond(id, engine.processExecutablePathJson(
                    static_cast<uint32_t>(std::max(0, extractInt(line, "processId", 0)))));
                continue;
            }

            if (line.find("\"configure\"") != std::string::npos) {
                const clipture::EngineSettings settings {
                    extractInt(line, "fps", 30),
                    extractInt(line, "bitrateMbps", 40),
                    extractInt(line, "nvencPreset", 3),
                    extractInt(line, "clipLengthSeconds", 30),
                    extractString(line, "monitorId").empty() ? "primary" : extractString(line, "monitorId"),
                    extractInt(line, "targetWidth", 0),
                    extractInt(line, "targetHeight", 0),
                    extractBool(line, "includeMixedAudio", true),
                    extractBool(line, "includeSystemAudio", true),
                    extractBool(line, "includeMicrophoneAudio", true),
                    extractBool(line, "captureGameAudio", false),
                    extractBool(line, "captureForegroundSystemAudio", false),
                    extractFloat(line, "micVolume", 1.0f),
                    extractBool(line, "micIsolation", false),
                    extractFloat(line, "micIsolationWeight", 1.0f),
                    extractBool(line, "noiseGateEnabled", true),
                    extractBool(line, "autoNoiseGate", true),
                    extractFloat(line, "noiseGateThreshold", 0.05f),
                    extractInt(line, "noiseGateDebounceMs", 180),
                    extractString(line, "micDeviceId"),
                    extractString(line, "micDeviceMatchKey"),
                    extractString(line, "micDeviceName"),
                    splitList(extractString(line, "appAudioProcesses")),
                    splitList(extractString(line, "systemAudioProcesses"))
                };
                respond(id, clipture::toJson(engine.configure(settings)));
                continue;
            }

            if (line.find("\"saveClip\"") != std::string::npos) {
                clipture::SaveClipResult result;
                {
                    SavePriorityGuard savePriority;
                    result = engine.saveClip({
                        extractDuration(line),
                        extractString(line, "saveFolder"),
                        extractBool(line, "analyzeIo", false)
                    });
                }
                std::ostringstream payload;
                payload << "{\"ok\":" << (result.ok ? "true" : "false") << ","
                        << "\"message\":\"" << jsonEscape(result.message) << "\"";
                if (!result.ioAnalysisJson.empty()) {
                    payload << ",\"saveIoAnalysis\":" << result.ioAnalysisJson;
                }
                if (result.ok) {
                    payload << ",\"clip\":" << result.clipJson;
                }
                payload << "}";
                respond(id, payload.str());
                continue;
            }

            respondError(id, "Unknown engine command.");
        } catch (const std::exception& error) {
            respondError(id, error.what());
        }
    }

    timeEndPeriod(1);
    return 0;
}
