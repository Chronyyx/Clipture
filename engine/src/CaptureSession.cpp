#include "clipture/CaptureSession.hpp"

#include "CaptureBackend.hpp"
#include "DesktopDuplicationBackend.hpp"
#include "WgcCaptureBackend.hpp"
#include "clipture/CaptureBackendPolicy.hpp"
#include "clipture/MediaClock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

namespace clipture {
namespace {

CaptureBackendPreference captureBackendPreferenceFromEnvironment() {
    std::string value = "auto";
    if (const char* configured = std::getenv("CLIPTURE_CAPTURE_BACKEND")) value = configured;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    bool valid = false;
    const auto preference = parseCaptureBackendPreference(value, valid);
    if (!valid) {
        static std::atomic<bool> warningLogged = false;
        bool expected = false;
        if (warningLogged.compare_exchange_strong(expected, true)) {
            std::cerr << "[capture] Ignoring invalid CLIPTURE_CAPTURE_BACKEND='"
                      << value << "'; expected auto, dxgi, or wgc.\n";
        }
    }
    return preference;
}

}  // namespace

struct CaptureSession::Impl {
    std::shared_ptr<capture::CaptureSharedState> shared =
        std::make_shared<capture::CaptureSharedState>();
    std::jthread controllerThread;
    std::recursive_mutex lifecycleMutex;
    std::mutex controllerMutex;
    std::string quarantinedMonitorKey;

    FrameQueue* configuredFrameQueue = nullptr;
    CaptureTickGate* configuredTickGate = nullptr;
    std::string configuredMonitorId = "primary";
    std::atomic<void*> activeWindow = nullptr;
    std::atomic<void*> failedWindow = nullptr;

    std::mutex initialMutex;
    std::condition_variable initialCv;
    bool initialComplete = false;
    bool initialSuccess = false;

    void completeInitial(bool success) {
        {
            std::lock_guard lock(initialMutex);
            if (initialComplete) return;
            initialComplete = true;
            initialSuccess = success;
        }
        initialCv.notify_all();
    }

    void quarantine(const std::string& monitorKey) {
        std::lock_guard lock(controllerMutex);
        quarantinedMonitorKey = monitorKey;
    }

    bool isQuarantinedForSelection(const std::string& monitorKey) {
        std::lock_guard lock(controllerMutex);
        if (!quarantinedMonitorKey.empty() && quarantinedMonitorKey != monitorKey) {
            quarantinedMonitorKey.clear();
        }
        return !monitorKey.empty() && quarantinedMonitorKey == monitorKey;
    }

    CaptureBackendKind monitorFallbackKind(
        const capture::SelectedOutput& output,
        CaptureBackendPreference preference) const {
        const bool identityRotation =
            output.desc.Rotation == DXGI_MODE_ROTATION_IDENTITY ||
            output.desc.Rotation == DXGI_MODE_ROTATION_UNSPECIFIED;
        if (preference == CaptureBackendPreference::Wgc || !identityRotation) {
            return CaptureBackendKind::Wgc;
        }
        return CaptureBackendKind::Dxgi;
    }

    void runController(
        std::stop_token stopToken,
        capture::SelectedOutput selectedOutput,
        std::string monitorId,
        CaptureBackendPreference preference,
        CaptureBackendKind initialKind,
        HWND initialWindow,
        std::string targetName) {
        CaptureBackendKind kind = initialKind;
        HWND captureWindow = initialWindow;
        bool backendHasRun = false;

        auto fallBackFromWindow = [&](const std::string& reason) {
            shared->setFallbackReason(reason);
            ++shared->fallbackCount;
            shared->beginEpoch();
            failedWindow.store(captureWindow, std::memory_order_release);
            activeWindow.store(nullptr, std::memory_order_release);
            captureWindow = nullptr;
            shared->setCaptureTarget("monitor", selectedOutput.displayName);
            kind = monitorFallbackKind(selectedOutput, preference);
            std::cerr << "[capture] Game-window capture unavailable; falling back to "
                      << captureBackendKindName(kind) << ": " << reason << '\n';
        };

        while (!stopToken.stop_requested()) {
            std::unique_ptr<capture::CaptureBackend> backend;
            if (kind == CaptureBackendKind::Dxgi) {
                backend = std::make_unique<capture::DesktopDuplicationBackend>(
                    shared, selectedOutput, monitorId);
            } else if (kind == CaptureBackendKind::Wgc) {
                backend = std::make_unique<capture::WgcCaptureBackend>(shared, selectedOutput);
            } else if (kind == CaptureBackendKind::WgcWindow) {
                backend = std::make_unique<capture::WgcCaptureBackend>(
                    shared, selectedOutput, captureWindow, targetName);
            } else {
                shared->setStatus("No capture backend was selected.");
                completeInitial(false);
                break;
            }

            const auto startResult = backend->start();
            if (!startResult.ok) {
                backend->stop();
                if (kind == CaptureBackendKind::WgcWindow) {
                    fallBackFromWindow(startResult.message);
                    continue;
                }
                if (kind == CaptureBackendKind::Dxgi && preference == CaptureBackendPreference::Auto) {
                    shared->setFallbackReason(startResult.message);
                    ++shared->fallbackCount;
                    quarantine(selectedOutput.monitorKey);
                    std::cerr << "[capture] DXGI startup failed; falling back to WGC: "
                              << startResult.message << '\n';
                    kind = CaptureBackendKind::Wgc;
                    continue;
                }
                shared->running.store(false, std::memory_order_release);
                shared->setStatus(startResult.message);
                completeInitial(false);
                break;
            }

            backendHasRun = true;
            completeInitial(true);
            const auto outcome = backend->run(stopToken);
            backend->stop();
            if (stopToken.stop_requested() || outcome == capture::BackendOutcome::Stopped) break;

            if (kind == CaptureBackendKind::WgcWindow) {
                const auto reason = shared->snapshot().fallbackReason;
                fallBackFromWindow(
                    reason.empty()
                        ? "Windows.Graphics.Capture game-window source stopped."
                        : reason);
                continue;
            }

            if (kind == CaptureBackendKind::Dxgi &&
                outcome == capture::BackendOutcome::RequestFallback &&
                preference == CaptureBackendPreference::Auto) {
                quarantine(selectedOutput.monitorKey);
                ++shared->fallbackCount;
                shared->beginEpoch();
                const auto fallback = shared->snapshot().fallbackReason;
                std::cerr << "[capture] DXGI recovery exhausted; switching to WGC"
                          << (fallback.empty() ? ".\n" : ": " + fallback + "\n");
                capture::SelectedOutput refreshedOutput;
                if (capture::selectOutput(monitorId, refreshedOutput)) {
                    selectedOutput = std::move(refreshedOutput);
                    shared->setSelectedOutput(selectedOutput);
                    shared->setCaptureTarget("monitor", selectedOutput.displayName);
                }
                kind = CaptureBackendKind::Wgc;
                continue;
            }

            shared->running.store(false, std::memory_order_release);
            if (outcome == capture::BackendOutcome::RequestFallback) {
                const auto reason = shared->snapshot().fallbackReason;
                shared->setStatus(
                    (reason.empty() ? "DXGI capture failed" : reason) +
                    "; automatic fallback is disabled by CLIPTURE_CAPTURE_BACKEND=dxgi.");
            }
            break;
        }

        if (!backendHasRun) completeInitial(false);
        shared->running.store(false, std::memory_order_release);
        activeWindow.store(nullptr, std::memory_order_release);
        {
            std::lock_guard lock(shared->stateMutex);
            shared->activeBackend = "none";
            shared->activeBackendStarted100ns = 0;
        }
    }

    void stopController(bool releaseBindings) {
        if (auto* tickGate = shared->captureTickGate.load(std::memory_order_acquire)) {
            tickGate->deactivate();
        }
        if (controllerThread.joinable()) {
            controllerThread.request_stop();
            controllerThread.join();
        }
        shared->running.store(false, std::memory_order_release);
        shared->hdrTonemappingActive.store(false, std::memory_order_relaxed);
        activeWindow.store(nullptr, std::memory_order_release);
        if (releaseBindings) {
            configuredFrameQueue = nullptr;
            configuredTickGate = nullptr;
            configuredMonitorId = "primary";
            failedWindow.store(nullptr, std::memory_order_release);
            shared->frameQueue.store(nullptr, std::memory_order_release);
            shared->captureTickGate.store(nullptr, std::memory_order_release);
            shared->activeMonitor.store(nullptr, std::memory_order_release);
        }
    }

    bool startTarget(HWND requestedWindow, std::string targetName) {
        stopController(false);

        capture::SelectedOutput selectedOutput;
        if (!capture::selectOutput(configuredMonitorId, selectedOutput) ||
            !selectedOutput.adapter || !selectedOutput.output) {
            shared->setStatus("No matching monitor was found for capture.");
            return false;
        }

        HWND captureWindow = requestedWindow;
        if (captureWindow &&
            (!IsWindow(captureWindow) ||
             MonitorFromWindow(captureWindow, MONITOR_DEFAULTTONULL) != selectedOutput.desc.Monitor)) {
            captureWindow = nullptr;
        }

        const auto preference = captureBackendPreferenceFromEnvironment();
        const bool identityRotation =
            selectedOutput.desc.Rotation == DXGI_MODE_ROTATION_IDENTITY ||
            selectedOutput.desc.Rotation == DXGI_MODE_ROTATION_UNSPECIFIED;
        const auto monitorDecision = decideCaptureBackend(
            preference,
            selectedOutput.hdrEnabled,
            identityRotation,
            isQuarantinedForSelection(selectedOutput.monitorKey));
        const CaptureBackendDecision decision =
            captureWindow && preference != CaptureBackendPreference::Dxgi
                ? CaptureBackendDecision {
                    CaptureBackendKind::WgcWindow,
                    true,
                    "foreground game uses non-injected window capture",
                }
                : monitorDecision;

        shared->resetForStart(
            configuredFrameQueue,
            configuredTickGate,
            selectedOutput,
            preference);
        shared->setCaptureTarget(
            captureWindow ? "game-window" : "monitor",
            captureWindow && !targetName.empty() ? targetName : selectedOutput.displayName);
        if (!decision.supported) {
            shared->setStatus(std::string(decision.reason));
            return false;
        }

        activeWindow.store(captureWindow, std::memory_order_release);
        {
            std::lock_guard lock(initialMutex);
            initialComplete = false;
            initialSuccess = false;
        }
        controllerThread = std::jthread(
            [this,
             selectedOutput,
             monitorId = configuredMonitorId,
             preference,
             kind = decision.kind,
             captureWindow,
             targetName = std::move(targetName)](std::stop_token stopToken) mutable {
                runController(
                    stopToken,
                    std::move(selectedOutput),
                    std::move(monitorId),
                    preference,
                    kind,
                    captureWindow,
                    std::move(targetName));
            });

        std::unique_lock initialLock(initialMutex);
        const bool completed = initialCv.wait_for(
            initialLock,
            std::chrono::seconds(5),
            [this] { return initialComplete; });
        const bool success = completed && initialSuccess;
        initialLock.unlock();
        if (!completed) {
            shared->setStatus("Capture backend startup timed out.");
            stopController(false);
        } else if (!success && controllerThread.joinable()) {
            controllerThread.join();
        }
        return success;
    }
};

CaptureSession::CaptureSession()
    : impl_(std::make_unique<Impl>()) {}

CaptureSession::~CaptureSession() {
    stop();
}

bool CaptureSession::startMonitor(
    FrameQueue* frameQueue,
    CaptureTickGate* captureTickGate,
    const std::string& monitorId) {
    std::lock_guard lock(impl_->lifecycleMutex);
    impl_->configuredFrameQueue = frameQueue;
    impl_->configuredTickGate = captureTickGate;
    impl_->configuredMonitorId = monitorId.empty() ? "primary" : monitorId;
    impl_->failedWindow.store(nullptr, std::memory_order_release);
    return impl_->startTarget(nullptr, {});
}

bool CaptureSession::preferGameWindow(void* window, const std::string& targetName) {
    auto captureWindow = static_cast<HWND>(window);
    if (!captureWindow || !IsWindow(captureWindow)) return false;

    std::lock_guard lock(impl_->lifecycleMutex);
    if (!impl_->configuredFrameQueue || !impl_->configuredTickGate) return false;
    if (impl_->activeWindow.load(std::memory_order_acquire) == captureWindow &&
        impl_->shared->running.load(std::memory_order_acquire)) {
        return true;
    }
    if (impl_->failedWindow.load(std::memory_order_acquire) == captureWindow &&
        impl_->shared->running.load(std::memory_order_acquire)) {
        return false;
    }

    impl_->failedWindow.store(nullptr, std::memory_order_release);
    return impl_->startTarget(captureWindow, targetName);
}

bool CaptureSession::preferMonitor() {
    std::lock_guard lock(impl_->lifecycleMutex);
    if (!impl_->configuredFrameQueue || !impl_->configuredTickGate) return false;
    if (impl_->activeWindow.load(std::memory_order_acquire) == nullptr &&
        impl_->shared->running.load(std::memory_order_acquire)) {
        return true;
    }
    impl_->failedWindow.store(nullptr, std::memory_order_release);
    return impl_->startTarget(nullptr, {});
}

void CaptureSession::setTargetFps(int fps) {
    impl_->shared->targetFps.store(std::clamp(fps, 1, 240), std::memory_order_relaxed);
}

void CaptureSession::stop() {
    std::lock_guard lock(impl_->lifecycleMutex);
    impl_->stopController(true);
}

bool CaptureSession::running() const {
    return impl_->shared->running.load(std::memory_order_acquire);
}

int CaptureSession::capturedFrames() const {
    return impl_->shared->capturedFrames.load(std::memory_order_relaxed);
}

int64_t CaptureSession::lastFrameInterval100ns() const {
    return impl_->shared->lastFrameInterval100ns.load(std::memory_order_relaxed);
}

int64_t CaptureSession::maximumFrameInterval100ns() const {
    return impl_->shared->maximumFrameInterval100ns.load(std::memory_order_relaxed);
}

int64_t CaptureSession::lastPublishedAge100ns() const {
    const int64_t publishedAt100ns = impl_->shared->lastPublishedSteady100ns.load(std::memory_order_acquire);
    if (publishedAt100ns <= 0) return 0;
    return std::max<int64_t>(0, monotonicNow100ns() - publishedAt100ns);
}

uint64_t CaptureSession::ownedSlotDrops() const {
    return impl_->shared->ownedSlotDrops.load(std::memory_order_relaxed);
}

uint64_t CaptureSession::sourceFramesSuperseded() const {
    return impl_->shared->sourceFramesSuperseded.load(std::memory_order_relaxed);
}

uint64_t CaptureSession::callbackErrors() const {
    return impl_->shared->callbackErrors.load(std::memory_order_relaxed);
}

uint64_t CaptureSession::captureEpoch() const {
    return impl_->shared->captureEpoch.load(std::memory_order_relaxed);
}

std::string CaptureSession::resolution() const {
    std::lock_guard lock(impl_->shared->stateMutex);
    return impl_->shared->resolution;
}

std::string CaptureSession::displayName() const {
    std::lock_guard lock(impl_->shared->stateMutex);
    return impl_->shared->displayName;
}

bool CaptureSession::hdrTonemappingActive() const {
    return impl_->shared->hdrTonemappingActive.load(std::memory_order_relaxed);
}

std::string CaptureSession::status() const {
    std::lock_guard lock(impl_->shared->stateMutex);
    return impl_->shared->status;
}

void* CaptureSession::activeMonitor() const {
    return impl_->shared->activeMonitor.load(std::memory_order_acquire);
}

CaptureRuntimeStats CaptureSession::runtimeStats() const {
    return impl_->shared->snapshot();
}

}  // namespace clipture
