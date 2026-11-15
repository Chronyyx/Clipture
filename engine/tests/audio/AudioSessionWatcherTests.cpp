// Live check of AudioSessionWatcher against the real audio stack. This process
// opens a render stream that plays digital silence (nothing audible) and
// times how fast the watcher reports its session starting and stopping. The
// bound sits well under the watcher's 1 s rescan, so passing it three times in
// a row takes the session notifications, not the backup rescan.
#include "clipture/AudioSessionWatcher.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto kNotificationBound = std::chrono::milliseconds(250);
constexpr int kRounds = 3;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

class SilentStream {
public:
    bool open() {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
            FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(client_.GetAddressOf())))) {
            return false;
        }
        WAVEFORMATEX* format = nullptr;
        if (FAILED(client_->GetMixFormat(&format))) return false;
        const HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2'000'000, 0, format, nullptr);
        CoTaskMemFree(format);
        return SUCCEEDED(hr) && SUCCEEDED(client_->GetService(IID_PPV_ARGS(&render_)));
    }

    void start() {
        UINT32 frames = 0;
        BYTE* data = nullptr;
        require(SUCCEEDED(client_->GetBufferSize(&frames)), "buffer size");
        require(SUCCEEDED(render_->GetBuffer(frames, &data)), "get buffer");
        require(SUCCEEDED(render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT)), "release buffer");
        require(SUCCEEDED(client_->Start()), "start stream");
    }

    void stop() {
        client_->Stop();
        client_->Reset();
    }

private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
};

struct Observed {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned notifications = 0;
};

bool contains(const clipture::AudioSessionWatcher& watcher, std::uint32_t processId) {
    const auto ids = watcher.soundingProcessIds();
    return std::find(ids.begin(), ids.end(), processId) != ids.end();
}

// Time until the watcher's set agrees with `wanted`, or the timeout.
std::chrono::milliseconds waitFor(
    const clipture::AudioSessionWatcher& watcher, Observed& observed, std::uint32_t processId, bool wanted,
    Clock::time_point since) {
    std::unique_lock lock(observed.mutex);
    observed.changed.wait_until(lock, since + std::chrono::seconds(3), [&] {
        return contains(watcher, processId) == wanted;
    });
    require(contains(watcher, processId) == wanted,
        wanted ? "watcher never reported the stream" : "watcher never dropped the stopped stream");
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since);
}

}  // namespace

int main() {
    try {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SilentStream stream;
        if (!stream.open()) {
            std::cout << "SKIP: no audio render device" << std::endl;
            return 0;
        }

        Observed observed;
        clipture::AudioSessionWatcher watcher([&] {
            std::lock_guard lock(observed.mutex);
            ++observed.notifications;
            observed.changed.notify_all();
        });
        const auto readyBy = Clock::now() + std::chrono::seconds(3);
        while (!watcher.ready() && Clock::now() < readyBy) Sleep(10);
        require(watcher.ready(), "watcher did not finish its first scan");

        const auto self = static_cast<std::uint32_t>(GetCurrentProcessId());
        require(!contains(watcher, self), "this process sounded before its stream started");

        for (int round = 1; round <= kRounds; ++round) {
            // Land each start at a different point of the rescan cycle.
            Sleep(137 * round);
            auto startedAt = Clock::now();
            stream.start();
            const auto appeared = waitFor(watcher, observed, self, true, startedAt);

            Sleep(100);
            const auto stoppedAt = Clock::now();
            stream.stop();
            const auto vanished = waitFor(watcher, observed, self, false, stoppedAt);

            std::cout << "round " << round << ": reported in " << appeared.count() << " ms, dropped in "
                      << vanished.count() << " ms" << std::endl;
            require(appeared < kNotificationBound, "start took the rescan path, not a notification");
            require(vanished < kNotificationBound, "stop took the rescan path, not a notification");
        }
        require(observed.notifications >= 2 * kRounds, "change callback missed a transition");
        std::cout << "PASS: audio session watcher" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
