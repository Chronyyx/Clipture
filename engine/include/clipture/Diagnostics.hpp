#pragma once

#include <string>
#include <utility>
#include <vector>

namespace clipture {

enum class EncoderName {
    Nvenc,
    MediaFoundationHardware,
    Software,
    Unavailable
};

struct Diagnostics {
    std::string captureApi = "Windows.Graphics.Capture";
    EncoderName activeEncoder = EncoderName::Unavailable;
    std::string encoderMode = "Unavailable";
    std::string gpu = "Unknown";
    std::string microphoneDevice = "Unknown";
    std::string display = "Primary display";
    bool hdrTonemapping = false;
    std::string videoSourceResolution = "Unknown";
    std::string videoOutputResolution = "Unknown";
    std::string videoScaling = "Unknown";
    std::string clipTargetResolution = "Unknown";
    std::string codec = "H.264";
    std::string resolution = "Native monitor";
    int fps = 30;
    int bitrateMbps = 40;
    bool hardwareAcceleration = false;
    int droppedFrames = 0;
    bool nvencAvailable = false;
    bool engineRunning = false;
    bool d3d11Ready = false;
    bool captureReady = false;
    bool audioReady = false;
    bool muxReady = false;
    int bufferedVideoPackets = 0;
    int bufferedAudioPackets = 0;
    int capturedFrames = 0;
    int queuedFrames = 0;
    int encoderAcceptedFrames = 0;
    int encoderOutputPackets = 0;
    int audioCapturedPackets = 0;
    int bufferDurationSeconds = 0;
    bool degraded = true;
    std::string status;
};

struct AudioInputDevice {
    std::string id;
    std::string name;
    bool isDefault = false;
    std::string state = "active";
    std::string matchKey;
};

struct DisplayDevice {
    std::string id;
    std::string name;
    bool isPrimary = false;
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    bool hdr = false;
};

Diagnostics collectDiagnostics();
std::string toJson(const Diagnostics& diagnostics);
std::string encoderName(EncoderName encoder);
std::string audioInputDevicesJson();
std::string displayDevicesJson();
std::pair<int, int> maxDisplayDimensions();
std::string microphoneDeviceName(const std::string& deviceId);
std::string audioEndpointMatchKey(const std::string& deviceId);

}  // namespace clipture
