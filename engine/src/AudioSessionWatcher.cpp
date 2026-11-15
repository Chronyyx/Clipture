#include "clipture/AudioSessionWatcher.hpp"

#include <windows.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace clipture {
namespace {

using Microsoft::WRL::ComPtr;

// Backs up the notifications; also catches a session that is not marked
// active but still meters sound.
constexpr DWORD kRescanIntervalMs = 1000;
// A rescan asks the sessions already held whether they sound. Listing every
// session again, which costs several times more, runs when Windows reports a
// new or ended session and, as backup, on every this-many-th rescan.
constexpr int kRescansPerListing = 3;
// A burst of notifications (one app opening several streams) becomes one scan.
constexpr DWORD kCoalesceMs = 10;
constexpr float kSoundingPeak = 0.0005f;

// `meter` is fetched on first use and kept with the session.
bool sessionIsSounding(IAudioSessionControl* session, ComPtr<IAudioMeterInformation>& meter) {
    AudioSessionState state = AudioSessionStateInactive;
    if (FAILED(session->GetState(&state))) return false;
    if (state == AudioSessionStateActive) return true;
    if (state == AudioSessionStateExpired) return false;
    float peak = 0.0f;
    if (!meter && FAILED(session->QueryInterface(IID_PPV_ARGS(&meter)))) return false;
    return meter && SUCCEEDED(meter->GetPeakValue(&peak)) && peak > kSoundingPeak;
}

std::wstring sessionInstanceId(IAudioSessionControl2* session) {
    LPWSTR raw = nullptr;
    if (FAILED(session->GetSessionInstanceIdentifier(&raw)) || !raw) return {};
    std::wstring id(raw);
    CoTaskMemFree(raw);
    return id;
}

// Owned jointly by the watcher thread and the COM callback object, which
// Windows may release after the watcher is gone.
struct Signal {
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> devicesChanged{false};
    // A session was created or ended: the held list is out of date.
    std::atomic<bool> sessionsChanged{false};
    ~Signal() {
        if (wake) CloseHandle(wake);
    }
    void poke(bool devices = false) {
        if (devices) devicesChanged = true;
        SetEvent(wake);
    }
    void pokeSessions() {
        sessionsChanged = true;
        SetEvent(wake);
    }
};

// Callbacks only signal the watcher thread: Windows forbids blocking or
// (un)registering inside them.
class Notifier final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::FtmBase,
          IMMNotificationClient,
          IAudioSessionNotification,
          IAudioSessionEvents> {
public:
    explicit Notifier(std::shared_ptr<Signal> signal) : signal_(std::move(signal)) {}

    // IMMNotificationClient
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { signal_->poke(true); return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { signal_->poke(true); return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { signal_->poke(true); return S_OK; }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { signal_->poke(true); return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    // IAudioSessionNotification
    STDMETHODIMP OnSessionCreated(IAudioSessionControl*) override { signal_->pokeSessions(); return S_OK; }

    // IAudioSessionEvents
    STDMETHODIMP OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnStateChanged(AudioSessionState state) override {
        if (state == AudioSessionStateExpired) signal_->pokeSessions();
        else signal_->poke();
        return S_OK;
    }
    STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override { signal_->pokeSessions(); return S_OK; }

private:
    std::shared_ptr<Signal> signal_;
};

}  // namespace

struct AudioSessionWatcher::State {
    std::function<void()> onChange;
    std::shared_ptr<Signal> signal = std::make_shared<Signal>();
    std::atomic<bool> stopping{false};
    std::atomic<bool> ready{false};
    mutable std::mutex mutex;
    std::vector<std::uint32_t> processIds;
    std::thread thread;

    void run();
};

namespace {

struct DeviceSessions {
    ComPtr<IAudioSessionManager2> manager;
};

struct WatchedSession {
    ComPtr<IAudioSessionControl> control;
    ComPtr<IAudioMeterInformation> meter;
    DWORD processId = 0;  // 0: no single process (system sounds)
    bool seen = false;
};

}  // namespace

void AudioSessionWatcher::State::run() {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(coHr);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        std::cerr << "[audio-sessions] Device enumerator unavailable; app tracks fall back to polling." << std::endl;
        if (coInitialized) CoUninitialize();
        return;
    }
    auto notifier = Microsoft::WRL::Make<Notifier>(signal);
    const bool endpointRegistered = SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(notifier.Get()));

    std::vector<DeviceSessions> devices;
    std::map<std::wstring, WatchedSession> sessions;
    std::vector<WatchedSession> unnamedSessions;

    auto releaseSessions = [&] {
        for (auto& [_, watched] : sessions) watched.control->UnregisterAudioSessionNotification(notifier.Get());
        sessions.clear();
        unnamedSessions.clear();
    };
    auto releaseDevices = [&] {
        releaseSessions();
        for (auto& device : devices) device.manager->UnregisterSessionNotification(notifier.Get());
        devices.clear();
    };
    auto attachDevices = [&] {
        releaseDevices();
        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) || !collection) return;
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT index = 0; index < count; ++index) {
            ComPtr<IMMDevice> device;
            ComPtr<IAudioSessionManager2> manager;
            if (FAILED(collection->Item(index, &device)) || !device ||
                FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(manager.GetAddressOf()))) || !manager) {
                continue;
            }
            // Session-created notifications start only after one enumeration.
            ComPtr<IAudioSessionEnumerator> primer;
            manager->GetSessionEnumerator(&primer);
            if (SUCCEEDED(manager->RegisterSessionNotification(notifier.Get()))) devices.push_back({ manager });
        }
    };
    // Brings the held sessions in line with what the devices list now.
    auto listSessions = [&] {
        unnamedSessions.clear();
        for (auto& [_, watched] : sessions) watched.seen = false;
        for (auto& device : devices) {
            ComPtr<IAudioSessionEnumerator> list;
            if (FAILED(device.manager->GetSessionEnumerator(&list)) || !list) continue;
            int count = 0;
            list->GetCount(&count);
            for (int index = 0; index < count; ++index) {
                ComPtr<IAudioSessionControl> control;
                ComPtr<IAudioSessionControl2> control2;
                if (FAILED(list->GetSession(index, &control)) || !control || FAILED(control.As(&control2))) continue;
                DWORD processId = 0;
                if (FAILED(control2->GetProcessId(&processId))) processId = 0;
                const auto id = sessionInstanceId(control2.Get());
                if (id.empty()) {
                    // Cannot be matched with an earlier listing: held only
                    // until the next one, without notifications.
                    unnamedSessions.push_back({ control, nullptr, processId, true });
                    continue;
                }
                auto [entry, inserted] = sessions.try_emplace(id);
                if (inserted) {
                    entry->second.control = control;
                    entry->second.processId = processId;
                    control->RegisterAudioSessionNotification(notifier.Get());
                }
                entry->second.seen = true;
            }
        }
        std::erase_if(sessions, [&](auto& item) {
            if (item.second.seen) return false;
            item.second.control->UnregisterAudioSessionNotification(notifier.Get());
            return true;
        });
    };
    // Which processes hold a session that sounds now.
    auto soundingProcesses = [&] {
        std::set<std::uint32_t> sounding;
        const auto add = [&](WatchedSession& watched) {
            if (watched.processId != 0 && sessionIsSounding(watched.control.Get(), watched.meter)) {
                sounding.insert(watched.processId);
            }
        };
        for (auto& [_, watched] : sessions) add(watched);
        for (auto& watched : unnamedSessions) add(watched);
        return std::vector<std::uint32_t>(sounding.begin(), sounding.end());
    };

    attachDevices();
    bool first = true;
    int rescansSinceListing = kRescansPerListing;
    while (!stopping) {
        bool listNow = signal->sessionsChanged.exchange(false);
        if (signal->devicesChanged.exchange(false)) {
            attachDevices();
            listNow = true;
        }
        if (listNow || ++rescansSinceListing >= kRescansPerListing) {
            listSessions();
            rescansSinceListing = 0;
        }
        auto current = soundingProcesses();
        bool changed = false;
        {
            std::lock_guard lock(mutex);
            changed = current != processIds;
            processIds = std::move(current);
        }
        if (first) {
            ready = true;
            first = false;
        }
        if (changed && onChange) onChange();

        WaitForSingleObject(signal->wake, kRescanIntervalMs);
        if (!stopping) Sleep(kCoalesceMs);
    }

    releaseDevices();
    if (endpointRegistered) enumerator->UnregisterEndpointNotificationCallback(notifier.Get());
    notifier.Reset();
    enumerator.Reset();
    if (coInitialized) CoUninitialize();
}

AudioSessionWatcher::AudioSessionWatcher(std::function<void()> onChange)
    : state_(std::make_shared<State>()) {
    state_->onChange = std::move(onChange);
    state_->thread = std::thread([state = state_] { state->run(); });
}

AudioSessionWatcher::~AudioSessionWatcher() {
    state_->stopping = true;
    state_->signal->poke();
    if (state_->thread.joinable()) state_->thread.join();
}

bool AudioSessionWatcher::ready() const {
    return state_->ready;
}

std::vector<std::uint32_t> AudioSessionWatcher::soundingProcessIds() const {
    std::lock_guard lock(state_->mutex);
    return state_->processIds;
}

}  // namespace clipture
