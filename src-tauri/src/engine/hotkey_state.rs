use crate::{contracts::HotkeyStatus, diagnostics::HotkeyDiagnostics, library::iso_utc};
use std::{sync::Mutex, time::SystemTime};

#[derive(Default)]
pub(super) struct HotkeyState(Mutex<HotkeyDiagnostics>);

impl HotkeyState {
    pub fn configuring(&self, configured: &str) {
        let mut state = self.0.lock().unwrap_or_else(|p| p.into_inner());
        state.configured = configured.into();
        state.normalized = configured.into();
        state.ready = false;
        state.armed = false;
        state.status = "Configuring native hotkey.".into();
    }
    pub fn configured(&self, status: &HotkeyStatus) {
        let mut state = self.0.lock().unwrap_or_else(|p| p.into_inner());
        state.ready = status.ready;
        state.armed = status.armed;
        state.status = status.status.clone();
    }
    pub fn unavailable(&self, message: &str) {
        let mut state = self.0.lock().unwrap_or_else(|p| p.into_inner());
        state.ready = false;
        state.armed = false;
        state.status = message.into();
    }
    pub fn triggered(&self, source: &str) {
        let mut state = self.0.lock().unwrap_or_else(|p| p.into_inner());
        state.trigger_count = state.trigger_count.saturating_add(1);
        state.last_trigger_at = Some(iso_utc(SystemTime::now()));
        state.last_trigger_source = Some(source.into());
    }
    pub fn snapshot(&self) -> HotkeyDiagnostics {
        self.0.lock().unwrap_or_else(|p| p.into_inner()).clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn engine_liveness_does_not_substitute_for_hotkey_registration() {
        let state = HotkeyState::default();
        state.configuring("Ctrl+F8");
        assert!(!state.snapshot().armed);
        state.configured(&HotkeyStatus {
            ready: true,
            armed: false,
            status: "Key unavailable".into(),
        });
        assert!(state.snapshot().ready);
        assert!(!state.snapshot().armed);
        state.triggered("raw-input");
        state.unavailable("Engine exited.");
        let snapshot = state.snapshot();
        assert!(!snapshot.ready);
        assert_eq!(snapshot.trigger_count, 1);
        assert_eq!(snapshot.last_trigger_source.as_deref(), Some("raw-input"));
        assert!(snapshot.last_trigger_at.is_some());
    }
}
