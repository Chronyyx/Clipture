//! Bridges sharing change hints to UI events and sharing moments to sound
//! cues. Emitting never blocks, and the payload is empty: the UI re-reads
//! the snapshot (events are hints). Cues play from the controller, so they
//! sound with no window open.
use std::sync::Arc;

use tauri::{AppHandle, Emitter};

use crate::{
    contracts::ClipRecord,
    sharing::{ShareCue, SharingEvents},
    sounds::{CuePlayer, Note},
};

pub const SHARING_CHANGED: &str = "sharing://changed";

pub struct TauriSharingEvents {
    pub app: AppHandle,
    /// `None` in test mode and on hosts without native playback.
    pub cues: Option<Arc<CuePlayer>>,
}

impl SharingEvents for TauriSharingEvents {
    fn changed(&self) {
        let _ = self.app.emit(SHARING_CHANGED, ());
    }

    fn library_changed(&self) {
        let _ = self.app.emit("library://changed", Option::<ClipRecord>::None);
    }

    fn cue(&self, cue: ShareCue) {
        if let Some(player) = &self.cues {
            player.play(&notes(cue));
        }
    }
}

const fn bell(hz: f32, at_seconds: f32, ring_seconds: f32, gain: f32) -> Note {
    Note {
        hz,
        at_seconds,
        ring_seconds,
        gain,
    }
}

/// Rising means good news, falling means a no, low means trouble.
fn notes(cue: ShareCue) -> Vec<Note> {
    match cue {
        // E5 then B5: a doorbell-like "something for you".
        ShareCue::Incoming => vec![bell(659.3, 0.0, 0.45, 0.9), bell(987.8, 0.12, 0.6, 1.0)],
        // G5 then C6, quick: a light "yes".
        ShareCue::Accepted => vec![bell(784.0, 0.0, 0.3, 0.8), bell(1046.5, 0.08, 0.45, 0.9)],
        // A C major arpeggio that rings out: done.
        ShareCue::Complete => vec![
            bell(523.3, 0.0, 0.35, 0.7),
            bell(659.3, 0.07, 0.35, 0.75),
            bell(784.0, 0.14, 0.4, 0.8),
            bell(1046.5, 0.21, 0.8, 1.0),
        ],
        // A4 falling to F4, softer: a polite "no".
        ShareCue::Declined => vec![bell(440.0, 0.0, 0.3, 0.8), bell(349.2, 0.14, 0.5, 0.7)],
        // Two short, low knocks: the line went quiet.
        ShareCue::Interrupted => vec![bell(293.7, 0.0, 0.18, 1.0), bell(293.7, 0.2, 0.25, 0.8)],
    }
}
