#pragma once

#include "CaptureBackend.hpp"

#include <memory>

namespace clipture::capture {

class WgcCaptureBackend final : public CaptureBackend {
public:
    WgcCaptureBackend(std::shared_ptr<CaptureSharedState> shared, SelectedOutput output);
    ~WgcCaptureBackend() override;

    BackendStartResult start() override;
    BackendOutcome run(std::stop_token stopToken) override;
    void stop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipture::capture
