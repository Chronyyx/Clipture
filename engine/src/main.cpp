#include "clipture/Engine.hpp"

#include <Windows.h>
#include <mmsystem.h>

#include "clipture/RawInputHotkey.hpp"
#include "clipture/protocol/JsonFields.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::mutex outputMutex;

using clipture::protocol::JsonFields;

int extractDuration(const JsonFields& request) {
    return static_cast<int>(request.integer("durationSeconds").value_or(30));
}

int extractInt(const JsonFields& request, const std::string& field, int fallback) {
    return static_cast<int>(request.integer(field).value_or(fallback));
}

bool extractBool(const JsonFields& request, const std::string& field, bool fallback) {
    return request.boolean(field).value_or(fallback);
}

std::string extractString(const JsonFields& request, const std::string& field) {
    return request.string(field).value_or(std::string{});
}

float extractFloat(const JsonFields& request, const std::string& field, float fallback) {
    return static_cast<float>(request.number(field).value_or(fallback));
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
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped.push_back(hex[(static_cast<unsigned char>(ch) >> 4) & 15]);
                    escaped.push_back(hex[static_cast<unsigned char>(ch) & 15]);
                } else {
                    escaped.push_back(ch);
                }
                break;
        }
    }
    return escaped;
}

void respondError(int id, const std::string& error) {
    std::lock_guard lock(outputMutex);
    std::cout << "{\"id\":" << id << ",\"error\":\"" << jsonEscape(error) << "\"}" << std::endl;
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
        belowNormalPrioritySet_ = SetThreadPriority(thread_, THREAD_PRIORITY_BELOW_NORMAL) != 0;
        std::cerr << "[save-timing] source=engine stage=save_priority"
                  << " previousPriority=" << previousPriority_
                  << " backgroundBegin=false"
                  << " adaptiveBackground=true"
                  << " belowNormal=" << (belowNormalPrioritySet_ ? "true" : "false")
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
    bool belowNormalPrioritySet_ = false;
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
        // One real parse per request: fields are looked up, never searched for,
        // so text inside a value cannot pose as another field or command.
        const auto parsed = JsonFields::parse(line);
        const int id = parsed ? static_cast<int>(parsed->integer("id").value_or(0)) : 0;
        if (!parsed) {
            respondError(id, "Malformed engine request.");
            continue;
        }
        const JsonFields& request = *parsed;
        const std::string type = request.string("type").value_or(std::string{});
        try {
            if (type == "getDiagnostics") {
                respond(id, clipture::toJson(engine.diagnostics()));
                continue;
            }

            if (type == "configureHotkey") {
                const bool armed = hotkey.configure(extractString(request, "hotkey"));
                std::ostringstream payload;
                payload << "{\"ready\":" << (hotkey.ready() ? "true" : "false")
                        << ",\"armed\":" << (armed ? "true" : "false")
                        << ",\"status\":\"" << jsonEscape(hotkey.status()) << "\"}";
                respond(id, payload.str());
                continue;
            }

            if (type == "listAudioInputDevices") {
                respond(id, clipture::audioInputDevicesJson());
                continue;
            }

            if (type == "listDisplayDevices") {
                respond(id, clipture::displayDevicesJson());
                continue;
            }

            if (type == "listRunningProcesses") {
                respond(id, engine.runningProcessesJson(extractBool(request, "includeExecutablePaths", false)));
                continue;
            }

            if (type == "getProcessExecutablePath") {
                respond(id, engine.processExecutablePathJson(
                    static_cast<uint32_t>(std::max(0, extractInt(request, "processId", 0)))));
                continue;
            }

            if (type == "configure") {
                const clipture::EngineSettings settings {
                    extractInt(request, "fps", 30),
                    extractInt(request, "bitrateMbps", 40),
                    extractInt(request, "nvencPreset", 3),
                    extractInt(request, "clipLengthSeconds", 30),
                    extractString(request, "monitorId").empty() ? "primary" : extractString(request, "monitorId"),
                    extractInt(request, "targetWidth", 0),
                    extractInt(request, "targetHeight", 0),
                    extractBool(request, "includeMixedAudio", true),
                    extractBool(request, "includeSystemAudio", true),
                    extractBool(request, "includeMicrophoneAudio", true),
                    extractBool(request, "captureGameAudio", false),
                    extractBool(request, "captureForegroundSystemAudio", false),
                    extractFloat(request, "micVolume", 1.0f),
                    extractBool(request, "micIsolation", false),
                    extractFloat(request, "micIsolationWeight", 1.0f),
                    extractBool(request, "noiseGateEnabled", true),
                    extractBool(request, "autoNoiseGate", true),
                    extractFloat(request, "noiseGateThreshold", 0.05f),
                    extractInt(request, "noiseGateDebounceMs", 180),
                    extractString(request, "micDeviceId"),
                    extractString(request, "micDeviceMatchKey"),
                    extractString(request, "micDeviceName"),
                    splitList(extractString(request, "appAudioProcesses")),
                    splitList(extractString(request, "systemAudioProcesses")),
                    extractBool(request, "saveInPlace", true),
                    extractBool(request, "saveInPlaceOverlap", true),
                    extractString(request, "saveFolder")
                };
                respond(id, clipture::toJson(engine.configure(settings)));
                continue;
            }

            if (type == "saveClip") {
                clipture::SaveClipResult result;
                {
                    SavePriorityGuard savePriority;
                    result = engine.saveClip({
                        extractDuration(request),
                        extractString(request, "saveFolder"),
                        extractBool(request, "analyzeIo", false)
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
