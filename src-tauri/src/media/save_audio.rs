/// The saved clip's audio tracks and their names. Every track the engine
/// recorded stays its own track, in the engine's order (ADR 0028: the game,
/// each app and the microphone apart), so saving never rewrites the file for
/// audio. Until 1.8.6 apps not chosen in settings were mixed into one "System
/// audio" track; with apps no longer chosen there, that merged every app and
/// re-muxed every saved clip.
pub struct AudioLayout {
    pub groups: Vec<Vec<usize>>,
    pub labels: Vec<String>,
}

impl AudioLayout {
    pub fn new(tracks: &[String]) -> Self {
        let labels = tracks
            .iter()
            .map(|raw| {
                if raw == "system-loopback-pcm" {
                    return "System audio".to_owned();
                }
                match raw.strip_prefix("app:").or_else(|| raw.strip_prefix("game:")) {
                    Some(name) if name.to_ascii_lowercase().ends_with(".exe") => name[..name.len() - 4].to_owned(),
                    Some(name) => name.to_owned(),
                    None => raw.clone(),
                }
            })
            .collect();
        Self { groups: (0..tracks.len()).map(|index| vec![index]).collect(), labels }
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

    #[test]
    fn every_track_stays_apart_in_the_engines_order() {
        let tracks = [
            "game:Deadlock.exe",
            "app:Discord.exe",
            "app:chrome.exe",
            "microphone-pcm",
        ]
        .map(String::from);
        let layout = AudioLayout::new(&tracks);
        assert_eq!(layout.groups, vec![vec![0], vec![1], vec![2], vec![3]]);
        assert_eq!(layout.labels, ["Deadlock", "Discord", "chrome", "microphone-pcm"]);
        assert!(!layout.needs_mix(), "no app is mixed into another track");
        assert!(!layout.needs_remap(), "a save never re-muxes the clip for audio");
    }

    #[test]
    fn the_legacy_system_track_is_named_but_not_moved() {
        let layout = AudioLayout::new(&["microphone-pcm".into(), "system-loopback-pcm".into(), "game:Game".into()]);
        assert_eq!(layout.labels, ["microphone-pcm", "System audio", "Game"]);
        assert!(!layout.needs_remap());
    }
}
