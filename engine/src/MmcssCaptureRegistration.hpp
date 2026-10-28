#pragma once

#include <Windows.h>
#include <avrt.h>

namespace clipture::capture {

class MmcssCaptureRegistration {
public:
    explicit MmcssCaptureRegistration(bool enabled, bool highCategory) {
        if (!enabled) return;
        if (highCategory) {
            // Game engines register their threads with MMCSS too; in the same
            // Medium category they starve acquisition under heavy effects and
            // DXGI merges the updates. The loop needs ~0.5 ms of CPU per frame,
            // so the High category costs the game nothing measurable.
            handle_ = AvSetMmThreadCharacteristicsW(L"DisplayPostProcessing", &taskIndex_);
            if (handle_) return;
        }
        handle_ = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex_);
        // NVENC uses the same D3D device at high MMCSS priority. Keeping
        // acquisition at normal Capture priority prevents priority inversion.
        if (handle_) AvSetMmThreadPriority(handle_, AVRT_PRIORITY_NORMAL);
    }
    ~MmcssCaptureRegistration() {
        if (handle_) AvRevertMmThreadCharacteristics(handle_);
    }

private:
    DWORD taskIndex_ = 0;
    HANDLE handle_ = nullptr;
};

}  // namespace clipture::capture
