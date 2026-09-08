use crate::contracts::{AudioSourceKind, ClipSettings};

pub struct AudioLayout {
    pub groups: Vec<Vec<usize>>,
    pub labels: Vec<String>,
}

impl AudioLayout {
    pub fn new(tracks: &[String], settings: &ClipSettings) -> Self {
        let separate: Vec<_> = settings
            .audio_sources
            .iter()
            .filter(|source| source.kind == AudioSourceKind::App && source.enabled)
            .filter_map(|source| source.process_name.as_ref())
            .map(|name| format!("app:{name}"))
            .collect();
        let mut system = Vec::new();
        let mut microphones = Vec::new();
        let mut remaining = Vec::new();
        for (index, track) in tracks.iter().enumerate() {
            if track == "microphone-pcm" {
                microphones.push(index);
            } else if track == "system-loopback-pcm"
                || (track.starts_with("app:") && !separate.contains(track))
            {
                system.push(index);
            } else {
                remaining.push(index);
            }
        }
        let mut groups = Vec::new();
        let mut labels = Vec::new();
        if !system.is_empty() {
            groups.push(system);
            labels.push("System audio".into());
        }
        for index in microphones.into_iter().chain(remaining) {
            groups.push(vec![index]);
            let raw = &tracks[index];
            let name = raw
                .strip_prefix("app:")
                .or_else(|| raw.strip_prefix("game:"));
            labels.push(match name {
                Some(name) if name.to_ascii_lowercase().ends_with(".exe") => {
                    name[..name.len() - 4].into()
                }
                Some(name) => name.into(),
                None => raw.clone(),
            });
        }
        Self { groups, labels }
    }

    pub fn needs_mix(&self) -> bool {
        self.groups.iter().any(|group| group.len() > 1)
    }

    pub fn needs_remap(&self) -> bool {
        self.groups
            .iter()
            .enumerate()
            .any(|(index, group)| group.as_slice() != [index])
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::contracts::AudioSourceRule;

    #[test]
    fn system_mix_is_first_and_explicit_apps_remain_separate() {
        let mut settings = ClipSettings::default();
        settings.audio_sources.push(AudioSourceRule {
            kind: AudioSourceKind::App,
            enabled: true,
            process_name: Some("chat.exe".into()),
            ..AudioSourceRule::default()
        });
        let tracks = [
            "microphone-pcm",
            "app:browser.exe",
            "app:chat.exe",
            "system-loopback-pcm",
            "game:Game.EXE",
        ]
        .map(String::from);
        let layout = AudioLayout::new(&tracks, &settings);
        assert_eq!(layout.groups, vec![vec![1, 3], vec![0], vec![2], vec![4]]);
        assert_eq!(
            layout.labels,
            ["System audio", "microphone-pcm", "chat", "Game"]
        );
        assert!(layout.needs_mix());
        assert!(layout.needs_remap());
    }

    #[test]
    fn label_only_changes_do_not_require_ffmpeg_but_reordering_does() {
        let settings = ClipSettings::default();
        assert!(!AudioLayout::new(
            &["system-loopback-pcm".into(), "microphone-pcm".into()],
            &settings
        )
        .needs_remap());
        assert!(AudioLayout::new(
            &["microphone-pcm".into(), "system-loopback-pcm".into()],
            &settings
        )
        .needs_remap());
    }
}
