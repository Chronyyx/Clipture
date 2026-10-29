#pragma once

#include "clipture/ToneCurveControl.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <string>
#include <memory>

namespace clipture {
class GpuStageProbe;

class Tonemapper {
public:
    Tonemapper(Microsoft::WRL::ComPtr<ID3D11Device> device);
    ~Tonemapper();

    bool Initialize(std::string& errorMsg, float sdrWhiteLevel = 2.5f);

    // Tonemaps a 16-bit float input view to an 8-bit output view of the same
    // size. The caller owns the views, so freeing a texture is never delayed
    // by this class: a view keeps its texture (and the driver's system-memory
    // backing for it) alive. The pass also measures the frame's peak, read back
    // without waiting a few frames later, to pick the curve (ToneCurveControl).
    bool Process(
        ID3D11ShaderResourceView* inputFloat16,
        ID3D11UnorderedAccessView* outputUnorm8,
        UINT width,
        UINT height,
        std::string& errorMsg
    );
    // Convenience for callers without long-lived views; creates them per call.
    bool Process(
        Microsoft::WRL::ComPtr<ID3D11Texture2D> inputFloat16,
        Microsoft::WRL::ComPtr<ID3D11Texture2D> outputUnorm8,
        std::string& errorMsg
    );

    // Tests: a fixed knee instead of the measured one.
    void holdKnee(float knee) { heldKnee_ = knee; }
    // Whether the last measured frames had content above SDR white.
    bool hdrContent() const { return hdrLogged_; }

private:
    void readPeaks(int64_t now100ns);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> computeShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> peak_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> peakView_;
    struct PeakReadback {
        Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
        uint64_t order = 0;  // 0 = free
    };
    std::array<PeakReadback, 4> readbacks_;
    uint64_t nextOrder_ = 1;
    ToneCurveControl control_;
    float invWhite_ = 1.0f;
    float heldKnee_ = 0.0f;
    float appliedKnee_ = -1.0f;
    UINT appliedWidth_ = 0;
    UINT appliedHeight_ = 0;
    bool hdrLogged_ = false;
    std::unique_ptr<GpuStageProbe> timingProbe_;
};

} // namespace clipture
