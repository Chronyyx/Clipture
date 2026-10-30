//! Hotkey in admin games (ADR 0024). Windows keeps a game that runs as
//! administrator out of reach of the unelevated engine's hotkey. After the
//! user allows it once at Windows' prompt, the engine starts an elevated relay
//! (a copy of itself in Program Files, run by a scheduled task) while such a
//! game is open. This module owns the state the UI shows, the one in-game
//! notice per game and session, and the session's "Not now".
pub mod commands;

use std::{
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, Ordering},
        Mutex,
    },
};

use serde::{Deserialize, Serialize};

use crate::contracts::{ClipSettings, EngineDiagnostics};

#[derive(Clone, Debug, Default, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AdminGameHotkey {
    /// The game in front running as administrator, kept a minute after it closes.
    pub game: Option<String>,
    pub enabled: bool,
    /// The relay is set up on this PC (Windows' prompt was allowed).
    pub installed: bool,
    /// off, not-installed, idle, starting, active, outdated, rejected or failed.
    pub relay: String,
    /// "Not now" was chosen this session.
    pub dismissed: bool,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum AdminGameHotkeyOutcome {
    Allowed,
    /// Turned off from the setting; still set up for turning on again.
    Disabled,
    Removed,
    /// Windows' administrator prompt was declined.
    Declined,
    Failed,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AdminGameHotkeyChange {
    pub outcome: AdminGameHotkeyOutcome,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub message: Option<String>,
}

#[derive(Default)]
pub struct AdminHotkeyService {
    dismissed: AtomicBool,
    noticed: Mutex<Vec<String>>,
}

impl AdminHotkeyService {
    pub fn snapshot(&self, settings: &ClipSettings, diagnostics: &EngineDiagnostics) -> AdminGameHotkey {
        AdminGameHotkey {
            game: admin_game(diagnostics),
            enabled: settings.admin_game_hotkey,
            installed: relay_installed(),
            relay: detail(diagnostics, "adminHotkeyRelay").unwrap_or_else(|| "off".into()),
            dismissed: self.dismissed.load(Ordering::Relaxed),
        }
    }

    pub fn dismiss(&self) {
        self.dismissed.store(true, Ordering::Relaxed);
    }

    /// The admin game to tell the player about in game: once per game and
    /// session, only while the hotkey cannot reach it.
    pub fn notice_due(&self, settings: &ClipSettings, diagnostics: &EngineDiagnostics) -> Option<String> {
        let game = admin_game(diagnostics)?;
        let reaches = settings.admin_game_hotkey && relay_installed();
        if reaches || self.dismissed.load(Ordering::Relaxed) {
            return None;
        }
        let mut noticed = self.noticed.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        if noticed.contains(&game) {
            return None;
        }
        noticed.push(game.clone());
        Some(game)
    }
}

pub(super) fn detail(diagnostics: &EngineDiagnostics, key: &str) -> Option<String> {
    diagnostics.details.get(key).and_then(|value| value.as_str()).filter(|text| !text.is_empty()).map(str::to_owned)
}

fn admin_game(diagnostics: &EngineDiagnostics) -> Option<String> {
    detail(diagnostics, "adminGame")
}

/// The relay copy the elevated setup places; only administrators can write there.
pub(crate) fn relay_executable() -> Option<PathBuf> {
    let program_files = std::env::var_os("ProgramW6432").or_else(|| std::env::var_os("ProgramFiles"))?;
    Some(PathBuf::from(program_files).join("Clipture Hotkey Relay").join("clipture_engine.exe"))
}

pub(crate) fn relay_installed() -> bool {
    relay_executable().is_some_and(|path| path.is_file())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn diagnostics(game: &str) -> EngineDiagnostics {
        let mut diagnostics = EngineDiagnostics::unavailable("test");
        diagnostics.details.insert("adminGame".into(), json!(game));
        diagnostics.details.insert("adminHotkeyRelay".into(), json!("off"));
        diagnostics
    }

    #[test]
    fn the_in_game_notice_shows_once_per_game_and_never_after_not_now() {
        let service = AdminHotkeyService::default();
        let settings = ClipSettings::default();
        assert_eq!(service.notice_due(&settings, &diagnostics("")), None);
        assert_eq!(service.notice_due(&settings, &diagnostics("Marvel Rivals")).as_deref(), Some("Marvel Rivals"));
        assert_eq!(service.notice_due(&settings, &diagnostics("Marvel Rivals")), None);
        assert_eq!(service.notice_due(&settings, &diagnostics("Other Game")).as_deref(), Some("Other Game"));
        service.dismiss();
        assert_eq!(service.notice_due(&settings, &diagnostics("Third Game")), None);
        assert!(service.snapshot(&settings, &diagnostics("Third Game")).dismissed);
    }

    #[test]
    fn the_snapshot_serializes_for_the_renderer() {
        let snapshot = AdminHotkeyService::default().snapshot(&ClipSettings::default(), &diagnostics("Marvel Rivals"));
        let value = serde_json::to_value(&snapshot).unwrap();
        assert_eq!(value["game"], "Marvel Rivals");
        assert_eq!(value["relay"], "off");
        assert_eq!(value["enabled"], false);
        let change = AdminGameHotkeyChange { outcome: AdminGameHotkeyOutcome::Declined, message: None };
        assert_eq!(serde_json::to_value(change).unwrap(), json!({ "outcome": "declined" }));
    }
}
