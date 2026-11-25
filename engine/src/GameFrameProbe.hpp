#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>

namespace clipture::capture {

// Whether a captured game-window frame shows anything. Some games hide their
// window from capture or render outside it, which window capture returns as
// black. Reads back five small patches (centre and quadrants); `staging` is
// reused between calls. Waits for the GPU, so call it rarely.
bool gameFrameHasContent(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame,
    UINT width, UINT height, Microsoft::WRL::ComPtr<ID3D11Texture2D>& staging);

// The pixel test behind both probes, on read-back patches: any colour channel
// above near-black. `half` means R16G16B16A16_FLOAT (scRGB), else B8G8R8A8.
bool patchesHaveContent(const uint8_t* data, UINT rowPitch, UINT width, UINT height, bool half);

// Watches frames that are already being recorded, without stalling capture:
// request() queues the patch copy, poll() maps it only once the GPU has
// finished (D3D11_MAP_FLAG_DO_NOT_WAIT) and returns whether it had content.
class GameFrameWatch {
public:
    // Queues a probe of `frame`; false if one is already pending or the frame
    // cannot be probed.
    bool request(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame, UINT width, UINT height);
    // The verdict of the pending probe, or nothing while the GPU is still busy.
    std::optional<bool> poll(ID3D11DeviceContext* context);
    bool pending() const { return pending_; }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    bool pending_ = false;
    bool half_ = false;
};

// A recorded game window has gone black once this many probes in a row saw
// nothing. Two avoids reacting to one dark frame (a cut, a death fade).
class BlankWindowDetector {
public:
    static constexpr int kBlankVerdicts = 2;
    // Returns true when the window should be treated as blank.
    bool observe(bool content) {
        blank_ = content ? 0 : blank_ + 1;
        return blank_ >= kBlankVerdicts;
    }
    void reset() { blank_ = 0; }

private:
    int blank_ = 0;
};

}  // namespace clipture::capture
