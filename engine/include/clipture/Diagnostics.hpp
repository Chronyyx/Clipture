#pragma once

#include <cstdint>
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
    std::string requestedCaptureBackend = "auto";
    std::string activeCaptureBackend = "none";
    std::string captureFallbackReason;
    uint32_t displayRefreshNumerator = 0;
    uint32_t displayRefreshDenominator = 1;
    double displayRefreshHz = 0.0;
    uint64_t captureAcquiredUpdates = 0;
    uint64_t captureDesktopPresents = 0;
    uint64_t capturePointerUpdates = 0;
    uint64_t capturePublishedFrames = 0;
    uint64_t captureAccumulatedFrames = 0;
    uint64_t captureAccumulationEvents = 0;
    uint64_t captureSamplerRejections = 0;
    uint64_t captureNonMonotonicTimestamps = 0;
    uint64_t captureAcquireTimeouts = 0;
    uint64_t captureAccessLosses = 0;
    uint64_t captureRecreationAttempts = 0;
    uint64_t captureRecreationSuccesses = 0;
    uint64_t captureFallbacks = 0;
    double desktopPresentFps = 0.0;
    double publishedFreshFps = 0.0;
    double encodedRepeatRatio = 0.0;
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
    int captureOverflowDrops = 0;
    int captureCoalescedDrops = 0;
    int sourceFramesSuperseded = 0;
    int captureSlotDrops = 0;
    int captureCallbackErrors = 0;
    int schedulerDroppedFrames = 0;
    int schedulerRepeatedFrames = 0;
    int encoderQueueDrops = 0;
    int nvencSurfaceDrops = 0;
    int nvencInputDrops = 0;
    int encoderBackpressureDrops = 0;
    int nvencInFlightFrames = 0;
    int64_t maximumCaptureGap100ns = 0;
    int64_t maximumSubmitLatency100ns = 0;
    int64_t averageScaleLatency100ns = 0;
    int64_t maximumScaleLatency100ns = 0;
    int64_t averageInputMapLatency100ns = 0;
    int64_t maximumInputMapLatency100ns = 0;
    int64_t averageNvencCallLatency100ns = 0;
    int64_t maximumNvencCallLatency100ns = 0;
    int64_t averageOutputDrainLatency100ns = 0;
    int64_t maximumOutputDrainLatency100ns = 0;
    uint64_t captureEpoch = 0;
    std::string capturePressure = "healthy";
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
