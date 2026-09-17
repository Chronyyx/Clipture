#pragma once
#include "PipelineTimingTrace.hpp"
#include <d3d11_4.h>
#include <wrl/client.h>
#include <array>

namespace clipture {
// Opt-in sparse GPU timestamps, 8 pending samples/stage, at most 10 samples/s.
// No flush, event, polling loop or blocking readback is introduced. Each sample
// owns a disjoint query. The context lock prevents overlapping disjoint scopes
// from the capture and submit threads. Instrumented vs uninstrumented runs are
// necessary: even sparse timestamp packets/context locking can perturb work.
class GpuStageProbe {
    template<typename T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Slot { Ptr<ID3D11Query> disjoint, start, end; bool pending = false; int64_t issued = 0; };
public:
    explicit GpuStageProbe(const char* name) : timing_(name, "gpu-sampled"), name_(name) {}
    GpuStageProbe(const GpuStageProbe&) = delete;
    GpuStageProbe& operator=(const GpuStageProbe&) = delete;
    GpuStageProbe(GpuStageProbe&&) = default;
    GpuStageProbe& operator=(GpuStageProbe&&) = default;
    class Scope {
    public:
        Scope(GpuStageProbe* owner, ID3D11DeviceContext* context)
            : owner_(owner && owner->begin(context) ? owner : nullptr) {}
        Scope(GpuStageProbe& owner, ID3D11DeviceContext* context) : Scope(&owner, context) {}
        ~Scope() { if (owner_) owner_->end(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        GpuStageProbe* owner_;
    };
    Scope scope(ID3D11DeviceContext* context) { return Scope(*this, context); }
    uint64_t completed() const { return completed_; }
    uint64_t skipped() const { return skipped_; }
private:
    bool begin(ID3D11DeviceContext* context) {
        if (!pipelineTimingEnabled() || !context || disabled_) return false;
        const auto now = monotonicNow100ns();
        if (now < nextSample_) return false;
        nextSample_ = now + 1'000'000;
        Ptr<ID3D11Device> device;
        context->GetDevice(&device);
        if (device.Get() != device_.Get()) {
            slots_ = {}; device_ = device; context_ = context;
            multithread_.Reset(); context_.As(&multithread_);
        }
        if (!multithread_) { disabled_ = true; return false; }
        multithread_->Enter();
        for (auto& slot : slots_) {
            if (!slot.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
            UINT64 start = 0, end = 0;
            constexpr auto flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            const auto a = context_->GetData(slot.disjoint.Get(), &disjoint, sizeof(disjoint), flags);
            const auto b = context_->GetData(slot.start.Get(), &start, sizeof(start), flags);
            const auto c = context_->GetData(slot.end.Get(), &end, sizeof(end), flags);
            if (FAILED(a) || FAILED(b) || FAILED(c)) { slot.pending = false; ++skipped_; }
            else if (a == S_OK && b == S_OK && c == S_OK) {
                slot.pending = false;
                if (!disjoint.Disjoint && disjoint.Frequency && end >= start) {
                    timing_.record(static_cast<int64_t>((end - start) * (10'000'000.0 / disjoint.Frequency)));
                    ++completed_;
                } else ++skipped_;
            }
        }
        if (now - lastHealth_ >= 10'000'000) {
            int pending = 0;
            int64_t oldest = now;
            for (const auto& slot : slots_) if (slot.pending) { ++pending; oldest = std::min(oldest, slot.issued); }
            std::ostringstream out;
            out << "[pipeline-trace] {\"stage\":\"" << name_ << "\",\"kind\":\"gpu-health\",\"utcMs\":"
                << (detail::preciseFileTime100ns() - 116444736000000000LL) / 10'000
                << ",\"completed\":" << completed_ << ",\"skipped\":" << skipped_
                << ",\"pending\":" << pending << ",\"oldestPendingMs\":" << (now - oldest) / 10'000.0 << "}\n";
            std::cerr << out.str();
            lastHealth_ = now;
        }
        for (auto& slot : slots_) {
            if (slot.pending) continue;
            if (!slot.disjoint) {
                D3D11_QUERY_DESC desc { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
                auto hr = device_->CreateQuery(&desc, &slot.disjoint);
                desc.Query = D3D11_QUERY_TIMESTAMP;
                if (SUCCEEDED(hr)) hr = device_->CreateQuery(&desc, &slot.start);
                if (SUCCEEDED(hr)) hr = device_->CreateQuery(&desc, &slot.end);
                if (FAILED(hr)) {
                    disabled_ = true;
                    std::cerr << "[pipeline-trace] GPU queries unavailable for " << name_ << '\n';
                    break;
                }
            }
            active_ = &slot;
            slot.issued = now;
            context_->Begin(slot.disjoint.Get());
            context_->End(slot.start.Get());
            return true;
        }
        ++skipped_;
        multithread_->Leave();
        return false;
    }
    void end() {
        context_->End(active_->end.Get());
        context_->End(active_->disjoint.Get());
        active_->pending = true; active_ = nullptr;
        multithread_->Leave();
    }
    PipelineTimingTrace timing_;
    const char* name_;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    Ptr<ID3D11Multithread> multithread_;
    std::array<Slot, 8> slots_ {};
    Slot* active_ = nullptr;
    int64_t nextSample_ = 0;
    int64_t lastHealth_ = 0;
    uint64_t completed_ = 0, skipped_ = 0;
    bool disabled_ = false;
};
}
