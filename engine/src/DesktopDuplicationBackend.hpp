#pragma once

#include "CaptureBackend.hpp"

#include <memory>

namespace clipture::capture {

class DesktopDuplicationBackend final : public CaptureBackend {
public:
    DesktopDuplicationBackend(
        std::shared_ptr<CaptureSharedState> shared,
        SelectedOutput output,
        std::string monitorId);
    ~DesktopDuplicationBackend() override;

    BackendStartResult start() override;
    BackendOutcome run(std::stop_token stopToken) override;
    void stop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipture::capture
