mod background;
mod capture_gate;
mod clock;
pub mod commands;
mod model;
mod service;
#[cfg(debug_assertions)]
pub(crate) mod smoke;
mod stream_download;

use std::sync::Arc;

use crate::{engine::EngineClient, save::SaveCoordinator};

pub(crate) use background::start as start_background_checks;
use capture_gate::{CaptureAwareUpdateGate, NativeCaptureActivity};
use service::UpdateService;

pub(crate) fn runtime_service(
    engine: Arc<EngineClient>,
    saves: Arc<SaveCoordinator>,
    test_mode: bool,
) -> UpdateService {
    let activity = Arc::new(NativeCaptureActivity::new(engine, saves));
    let gate = Arc::new(CaptureAwareUpdateGate::new(activity, test_mode));
    if test_mode {
        UpdateService::without_network_checks(gate, "Updates are disabled by CLIPTURE_TEST_MODE.")
    } else if cfg!(debug_assertions) {
        UpdateService::without_network_checks(gate, "Updates are checked in installed builds.")
    } else {
        UpdateService::new(gate)
    }
}

pub(crate) fn automatic_checks_enabled(test_mode: bool) -> bool {
    !test_mode && !cfg!(debug_assertions)
}
