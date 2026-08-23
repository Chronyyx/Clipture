#pragma once

#include "clipture/CaptureBackendPolicy.hpp"
#include "clipture/CaptureSession.hpp"
#include "clipture/CaptureTickGate.hpp"
#include "clipture/FrameQueue.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace clipture::capture {

struct SelectedOutput {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC desc {};
    std::string displayName;
    std::string monitorKey;
    bool hdrEnabled = false;
    uint32_t refreshNumerator = 0;
    uint32_t refreshDenominator = 1;
};

bool selectOutput(const std::string& requestedId, SelectedOutput& selected);
float monitorSdrWhiteLevel(HMONITOR monitor);
std::string narrow(const wchar_t* value);
std::string hresultHex(HRESULT value);
HRESULT createD3dDeviceForOutput(
    IDXGIAdapter1* adapter,
    Microsoft::WRL::ComPtr<ID3D11Device>& device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>& context);

struct CaptureSharedState {
    std::atomic<int> capturedFrames = 0;
    std::atomic<int64_t> lastFramePts100ns = 0;
    std::atomic<int64_t> lastFrameInterval100ns = 0;
    std::atomic<int64_t> maximumFrameInterval100ns = 0;
    std::atomic<int64_t> lastPublishedPts100ns = 0;
    std::atomic<int64_t> lastPublishedSteady100ns = 0;
    std::atomic<uint64_t> captureEpoch = 0;
    std::atomic<uint64_t> frameSequence = 0;
    std::atomic<uint64_t> ownedSlotDrops = 0;
    std::atomic<uint64_t> sourceFramesSuperseded = 0;
    std::atomic<uint64_t> callbackErrors = 0;
    std::atomic<uint64_t> acquiredUpdates = 0;
    std::atomic<uint64_t> desktopPresents = 0;
    std::atomic<uint64_t> pointerUpdates = 0;
    std::atomic<uint64_t> publishedFrames = 0;
    std::atomic<uint64_t> accumulatedFrames = 0;
    std::atomic<uint64_t> accumulationEvents = 0;
    std::atomic<uint64_t> samplerRejections = 0;
    std::atomic<uint64_t> nonMonotonicTimestamps = 0;
    std::atomic<uint64_t> acquireTimeouts = 0;
    std::atomic<uint64_t> acquireImmediateMisses = 0;
    std::atomic<uint64_t> acquireGraceHits = 0;
    std::atomic<uint64_t> acquireGraceTimeouts = 0;
    std::atomic<uint64_t> accessLosses = 0;
    std::atomic<uint64_t> recreationAttempts = 0;
    std::atomic<uint64_t> recreationSuccesses = 0;
    std::atomic<uint64_t> fallbackCount = 0;
    std::atomic<bool> running = false;
    std::atomic<bool> hdrTonemappingActive = false;
    std::atomic<void*> activeMonitor = nullptr;
    std::atomic<int> targetFps = 60;
    std::atomic<FrameQueue*> frameQueue = nullptr;
    std::atomic<CaptureTickGate*> captureTickGate = nullptr;
    LatencyWindow<> acquireWaitLatency;
    LatencyWindow<> sourceUpdateInterval;
    LatencyWindow<> publishedPtsInterval;
    LatencyWindow<> publishedWallInterval;
    LatencyWindow<> framePreparationLatency;
    LatencyWindow<> cursorCompositeLatency;
    LatencyWindow<> frameProcessingLatency;

    mutable std::mutex stateMutex;
    std::string resolution = "Native monitor";
    std::string displayName = "Primary display";
    std::string status = "Capture session has not started.";
    std::string requestedBackend = "auto";
    std::string activeBackend = "none";
    std::string fallbackReason;
    std::string targetKind = "monitor";
    std::string targetName = "Primary display";
    uint32_t refreshNumerator = 0;
    uint32_t refreshDenominator = 1;
    int64_t activeBackendStarted100ns = 0;
    uint64_t activeBackendPresentBaseline = 0;
    uint64_t activeBackendPublishedBaseline = 0;
    mutable double smoothedFreshFps = 0.0;
    mutable int64_t lastSmoothedFreshTime100ns = 0;

    void resetForStart(
        FrameQueue* queue,
        CaptureTickGate* tickGate,
        const SelectedOutput& output,
        CaptureBackendPreference preference);
    void beginEpoch(bool clearQueue = true);
    void setStatus(std::string nextStatus);
    void setFallbackReason(std::string reason);
    void setSelectedOutput(const SelectedOutput& output);
    void setCaptureTarget(std::string kind, std::string name);
    void setActiveBackend(CaptureBackendKind kind);
    bool selectFrameTimestamp(
        int64_t sourceTimestamp100ns,
        int64_t& outputTimestamp100ns);
    void publish(
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
        std::shared_ptr<void> textureLease,
        int64_t timestamp100ns,
        int width,
        int height,
        bool sourceHadDesktopPresent,
        bool sourceHadPointerUpdate);
    CaptureRuntimeStats snapshot() const;
};

struct CaptureTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hdrInputTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView;
    std::shared_ptr<void> lease;
};

class CaptureTexturePool {
public:
    explicit CaptureTexturePool(std::shared_ptr<CaptureSharedState> shared);

    CaptureTexture acquire(
        ID3D11Device* device,
        UINT width,
        UINT height,
        bool needsUnorderedAccess,
        std::string& error);
    void reset();

private:
    struct Slot;
    std::shared_ptr<CaptureSharedState> shared_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<Slot>> slots_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hdrInputTexture_;
    std::size_t nextSlot_ = 0;
    D3D11_TEXTURE2D_DESC desc_ {};
};

enum class BackendOutcome {
    Stopped,
    Failed,
    RequestFallback,
};

struct BackendStartResult {
    bool ok = false;
    std::string message;
};

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual BackendStartResult start() = 0;
    virtual BackendOutcome run(std::stop_token stopToken) = 0;
    virtual void stop() = 0;
};

}  // namespace clipture::capture
