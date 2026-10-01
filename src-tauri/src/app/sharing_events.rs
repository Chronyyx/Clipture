//! Bridges sharing change hints to UI events. Emitting never blocks, and the
//! payload is empty: the UI re-reads the snapshot (events are hints).
use tauri::{AppHandle, Emitter};

use crate::{contracts::ClipRecord, sharing::SharingEvents};

pub const SHARING_CHANGED: &str = "sharing://changed";

pub struct TauriSharingEvents(pub AppHandle);

impl SharingEvents for TauriSharingEvents {
    fn changed(&self) {
        let _ = self.0.emit(SHARING_CHANGED, ());
    }

    fn library_changed(&self) {
        let _ = self.0.emit("library://changed", Option::<ClipRecord>::None);
    }
}
