use std::sync::Arc;

use crate::{contracts::CapturePressure, engine::EngineClient, save::SaveCoordinator};

use super::{model::UpdateOperation, service::UpdateGate};

/// Small read-only seam between the updater and capture ownership. It keeps
/// updater policy testable without giving the update domain access to AppState.
pub trait CaptureActivity: Send + Sync + 'static {
    fn pressure(&self) -> CapturePressure;
    fn save_in_progress(&self) -> bool;
    fn shutdown(&self) {}
    fn reserve_installation(&self) -> Result<Box<dyn Send>, String> {
        Ok(Box::new(()))
    }
    fn resume_after_failed_install(&self) {}
}

pub struct NativeCaptureActivity {
    engine: Arc<EngineClient>,
    saves: Arc<SaveCoordinator>,
}

impl NativeCaptureActivity {
    pub fn new(engine: Arc<EngineClient>, saves: Arc<SaveCoordinator>) -> Self {
        Self { engine, saves }
    }
}

impl CaptureActivity for NativeCaptureActivity {
    fn reserve_installation(&self) -> Result<Box<dyn Send>, String> {
        self.saves
            .reserve_installation()
            .map(|lease| Box::new(lease) as Box<dyn Send>)
            .ok_or_else(|| "A clip save or update installation is already in progress.".into())
    }
    fn resume_after_failed_install(&self) {
        self.engine.resume_after_failed_update();
    }
    fn shutdown(&self) {
        self.engine.shutdown();
    }
    fn pressure(&self) -> CapturePressure {
        self.engine.cached_diagnostics().capture_pressure
    }

    fn save_in_progress(&self) -> bool {
        self.saves.is_busy()
    }
}

pub struct CaptureAwareUpdateGate {
    activity: Arc<dyn CaptureActivity>,
    test_mode: bool,
}

impl CaptureAwareUpdateGate {
    pub fn new(activity: Arc<dyn CaptureActivity>, test_mode: bool) -> Self {
        Self {
            activity,
            test_mode,
        }
    }
}

impl UpdateGate for CaptureAwareUpdateGate {
    fn capture_pressure(&self) -> CapturePressure {
        self.activity.pressure()
    }
    fn reserve_installation(&self) -> Result<Box<dyn Send>, String> {
        if let Some(reason) = self.block_reason(UpdateOperation::Install) {
            return Err(reason);
        }
        self.activity.reserve_installation()
    }
    fn installation_failed(&self) {
        self.activity.resume_after_failed_install();
    }
    fn before_exit(&self) {
        self.activity.shutdown();
    }
    fn block_reason(&self, operation: UpdateOperation) -> Option<String> {
        if self.test_mode {
            return Some(
                "Updates are disabled for isolated profiles and CLIPTURE_TEST_MODE.".into(),
            );
        }
        if self.activity.save_in_progress() {
            return Some(format!(
                "Update {} deferred until the active clip save finishes.",
                operation.label()
            ));
        }
        match self.activity.pressure() {
            CapturePressure::Healthy => None,
            CapturePressure::Elevated | CapturePressure::Critical
                if operation == UpdateOperation::Download =>
            {
                None
            }
            CapturePressure::Elevated => Some(format!(
                "Update {} deferred until capture pressure returns to healthy.",
                operation.label()
            )),
            CapturePressure::Critical | CapturePressure::Unknown => Some(format!(
                "Update {} deferred because capture pressure is not safe.",
                operation.label()
            )),
        }
    }
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};

    use super::*;

    struct FakeActivity {
        pressure: AtomicU8,
        saving: AtomicBool,
    }

    impl FakeActivity {
        fn new(pressure: CapturePressure, saving: bool) -> Self {
            Self {
                pressure: AtomicU8::new(encode_pressure(pressure)),
                saving: AtomicBool::new(saving),
            }
        }
    }

    impl CaptureActivity for FakeActivity {
        fn pressure(&self) -> CapturePressure {
            decode_pressure(self.pressure.load(Ordering::Acquire))
        }

        fn save_in_progress(&self) -> bool {
            self.saving.load(Ordering::Acquire)
        }
    }

    fn encode_pressure(pressure: CapturePressure) -> u8 {
        match pressure {
            CapturePressure::Healthy => 0,
            CapturePressure::Elevated => 1,
            CapturePressure::Critical => 2,
            CapturePressure::Unknown => 3,
        }
    }

    fn decode_pressure(value: u8) -> CapturePressure {
        match value {
            0 => CapturePressure::Healthy,
            1 => CapturePressure::Elevated,
            2 => CapturePressure::Critical,
            _ => CapturePressure::Unknown,
        }
    }

    #[test]
    fn saves_block_transfer_and_pressure_blocks_install_but_allows_paced_downloads() {
        let activity = Arc::new(FakeActivity::new(CapturePressure::Healthy, false));
        let gate = CaptureAwareUpdateGate::new(activity.clone(), false);
        assert_eq!(gate.block_reason(UpdateOperation::Download), None);

        activity.saving.store(true, Ordering::Release);
        assert!(gate
            .block_reason(UpdateOperation::Install)
            .unwrap()
            .contains("active clip save"));

        activity.saving.store(false, Ordering::Release);
        activity.pressure.store(
            encode_pressure(CapturePressure::Elevated),
            Ordering::Release,
        );
        assert_eq!(gate.block_reason(UpdateOperation::Download), None);
        assert!(gate
            .block_reason(UpdateOperation::Install)
            .unwrap()
            .contains("capture pressure"));
    }

    #[test]
    fn test_mode_blocks_every_mutating_update_operation() {
        let activity = Arc::new(FakeActivity::new(CapturePressure::Healthy, false));
        let gate = CaptureAwareUpdateGate::new(activity, true);

        for operation in [UpdateOperation::Download, UpdateOperation::Install] {
            assert!(gate
                .block_reason(operation)
                .unwrap()
                .contains("CLIPTURE_TEST_MODE"));
        }
    }
}
