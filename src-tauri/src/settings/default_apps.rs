//! Separate tracks for apps most people want apart from game audio: the
//! default web browser and Discord. Added once, never re-added after removal,
//! and never altering sources the user already has.
use crate::contracts::{AudioSourceKind, AudioSourceRule, ClipSettings};

pub const DEFAULT_APP_SOURCES_VERSION: u32 = 1;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DetectedAudioApp {
    pub id: &'static str,
    pub label: String,
    pub process_name: String,
    pub executable_path: Option<String>,
}

/// Returns true when `settings` changed. The version marker is raised even
/// when nothing was detected, so an app installed later is not added silently.
pub fn seed_default_app_sources(settings: &mut ClipSettings, detected: &[DetectedAudioApp]) -> bool {
    if settings.default_app_sources_version >= DEFAULT_APP_SOURCES_VERSION {
        return false;
    }
    settings.default_app_sources_version = DEFAULT_APP_SOURCES_VERSION;
    for app in detected {
        let already_present = settings.audio_sources.iter().any(|source| {
            source.id == app.id
                || source
                    .process_name
                    .as_deref()
                    .is_some_and(|name| name.eq_ignore_ascii_case(&app.process_name))
        });
        if already_present {
            continue;
        }
        settings.audio_sources.push(AudioSourceRule {
            id: app.id.into(),
            label: app.label.clone(),
            kind: AudioSourceKind::App,
            process_name: Some(app.process_name.clone()),
            executable_path: app.executable_path.clone(),
            enabled: true,
            omit_if_silent: true,
            ..AudioSourceRule::default()
        });
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    fn app(id: &'static str, process: &str) -> DetectedAudioApp {
        DetectedAudioApp {
            id,
            label: process.trim_end_matches(".exe").into(),
            process_name: process.into(),
            executable_path: None,
        }
    }

    #[test]
    fn fresh_profiles_get_separate_browser_and_discord_tracks_once() {
        let mut settings = ClipSettings::default();
        let detected = [app("app-default-browser", "chrome.exe"), app("app-discord", "Discord.exe")];
        assert!(seed_default_app_sources(&mut settings, &detected));
        let apps: Vec<_> = settings.audio_sources.iter().filter(|s| s.kind == AudioSourceKind::App).collect();
        assert_eq!(apps.len(), 2);
        assert!(apps.iter().all(|source| source.enabled && source.omit_if_silent));
        assert_eq!(apps[1].process_name.as_deref(), Some("Discord.exe"));

        // A removed source stays removed, and the pass is not repeated.
        settings.audio_sources.retain(|source| source.id != "app-discord");
        assert!(!seed_default_app_sources(&mut settings, &detected));
        assert!(!settings.audio_sources.iter().any(|source| source.id == "app-discord"));
    }

    #[test]
    fn existing_sources_are_preserved_and_not_duplicated() {
        let mut settings = ClipSettings::default();
        settings.audio_sources.push(AudioSourceRule {
            id: "app-mine".into(),
            label: "My Discord".into(),
            kind: AudioSourceKind::App,
            process_name: Some("discord.EXE".into()),
            enabled: false,
            ..AudioSourceRule::default()
        });
        let before = settings.audio_sources.clone();
        assert!(seed_default_app_sources(&mut settings, &[app("app-discord", "Discord.exe")]));
        assert_eq!(settings.audio_sources, before, "the user's own disabled Discord track wins");
        assert_eq!(settings.default_app_sources_version, DEFAULT_APP_SOURCES_VERSION);
    }

    #[test]
    fn nothing_detected_still_completes_the_one_time_pass() {
        let mut settings = ClipSettings::default();
        assert!(seed_default_app_sources(&mut settings, &[]));
        assert_eq!(settings.default_app_sources_version, DEFAULT_APP_SOURCES_VERSION);
    }
}
