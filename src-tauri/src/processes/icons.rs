use std::{
    collections::{HashMap, HashSet, VecDeque},
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};

use crate::{contracts::ClipRecord, error::AppResult};

use super::ProcessService;

pub trait IconSource: Send + Sync {
    fn executable_icon_data_url(&self, executable: &Path, size: u32) -> AppResult<Option<String>>;

    fn installed_executable(&self, _candidate_names: &[String]) -> AppResult<Option<PathBuf>> {
        Ok(None)
    }
}

#[derive(Default)]
struct IconCache {
    values: HashMap<String, String>,
    order: VecDeque<String>,
}

pub struct ProcessIconService {
    processes: Arc<ProcessService>,
    source: Arc<dyn IconSource>,
    cache: Mutex<IconCache>,
    capacity: usize,
}

impl ProcessIconService {
    pub fn new(processes: Arc<ProcessService>, source: Arc<dyn IconSource>) -> Self {
        Self {
            processes,
            source,
            cache: Mutex::new(IconCache::default()),
            capacity: 128,
        }
    }

    /// The renderer's optional executable path is treated as a hint and is
    /// accepted only if it matches the process provider's latest snapshot.
    pub fn process_icon(
        &self,
        process_name: &str,
        requested_executable: Option<&str>,
    ) -> AppResult<String> {
        let candidates = candidate_names([process_name]);
        if candidates.is_empty() {
            return Ok(String::new());
        }
        let processes = self.processes.list()?;
        let requested_key = requested_executable.map(path_text_key);
        let active_path = processes
            .iter()
            .filter(|process| candidate_matches(&process.name, &candidates))
            .filter_map(|process| process.executable_path.as_deref())
            .find(|path| {
                requested_key
                    .as_deref()
                    .is_none_or(|wanted| path_text_key(path) == wanted)
            })
            .or_else(|| {
                processes
                    .iter()
                    .filter(|process| candidate_matches(&process.name, &candidates))
                    .find_map(|process| process.executable_path.as_deref())
            });
        let executable = active_path
            .map(PathBuf::from)
            .or(self.source.installed_executable(&candidates)?);
        self.icon_for_validated_executable(executable.as_deref(), 36)
    }

    pub fn clip_icon(&self, clip: &ClipRecord, preferred_labels: &[String]) -> AppResult<String> {
        let labels = clip_icon_candidates(clip, preferred_labels);
        if labels.is_empty() {
            return Ok(String::new());
        }
        let processes = self.processes.list()?;
        let executable = processes
            .iter()
            .find(|process| candidate_matches(&process.name, &labels))
            .and_then(|process| process.executable_path.as_deref())
            .map(PathBuf::from)
            .or(self.source.installed_executable(&labels)?);
        self.icon_for_validated_executable(executable.as_deref(), 20)
    }

    pub fn clear(&self) {
        *self
            .cache
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = IconCache::default();
    }

    fn icon_for_validated_executable(
        &self,
        executable: Option<&Path>,
        size: u32,
    ) -> AppResult<String> {
        let Some(executable) = executable else {
            return Ok(String::new());
        };
        if !executable.is_file()
            || !executable
                .extension()
                .and_then(|value| value.to_str())
                .is_some_and(|value| value.eq_ignore_ascii_case("exe"))
        {
            return Ok(String::new());
        }
        let canonical = executable
            .canonicalize()
            .unwrap_or_else(|_| executable.to_owned());
        let key = format!("{}|{size}", path_text_key(&canonical.to_string_lossy()));
        if let Some(value) = self.cached(&key) {
            return Ok(value);
        }
        let value = self
            .source
            .executable_icon_data_url(&canonical, size)?
            .unwrap_or_default();
        if !value.is_empty() && value.starts_with("data:image/") && value.len() <= 2 * 1024 * 1024 {
            self.insert(key, value.clone());
        }
        Ok(value)
    }

    fn cached(&self, key: &str) -> Option<String> {
        let mut cache = self
            .cache
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let value = cache.values.get(key).cloned()?;
        cache.order.retain(|candidate| candidate != key);
        cache.order.push_back(key.into());
        Some(value)
    }

    fn insert(&self, key: String, value: String) {
        let mut cache = self
            .cache
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        cache.order.retain(|candidate| candidate != &key);
        cache.order.push_back(key.clone());
        cache.values.insert(key, value);
        while cache.values.len() > self.capacity {
            if let Some(oldest) = cache.order.pop_front() {
                cache.values.remove(&oldest);
            }
        }
    }
}

fn clip_icon_candidates(clip: &ClipRecord, preferred_labels: &[String]) -> Vec<String> {
    if preferred_labels
        .iter()
        .any(|label| !label.trim().is_empty())
    {
        return candidate_names(preferred_labels.iter().map(String::as_str));
    }
    let tracks = clip.audio_tracks.iter().filter_map(|track| {
        (!matches!(
            track.as_str(),
            "system-loopback-pcm" | "microphone-pcm" | "mixed-preview-pcm"
        ))
        .then(|| track.trim_start_matches("app:").trim_start_matches("game:"))
    });
    candidate_names(
        std::iter::once(clip.game_or_app.as_str())
            .chain(clip.focused_apps.iter().flatten().map(String::as_str))
            .chain(tracks),
    )
}

fn candidate_names<'a>(values: impl IntoIterator<Item = &'a str>) -> Vec<String> {
    const IGNORED: &[&str] = &[
        "",
        "foreground app",
        "clipture",
        "system audio",
        "microphone",
        "mixed preview",
    ];
    let aliases = known_aliases();
    let mut seen = HashSet::new();
    let mut result = Vec::new();
    for value in values {
        let value = value.trim();
        let key = value.to_ascii_lowercase();
        if IGNORED.contains(&key.as_str()) || !seen.insert(key.clone()) {
            continue;
        }
        result.push(value.into());
        let stem = key.trim_end_matches(".exe");
        if let Some(extra) = aliases.get(stem) {
            for alias in *extra {
                if seen.insert((*alias).into()) {
                    result.push((*alias).into());
                }
            }
        }
    }
    result
}

fn candidate_matches(process_name: &str, candidates: &[String]) -> bool {
    let name = process_name
        .rsplit(['/', '\\'])
        .next()
        .unwrap_or(process_name)
        .to_ascii_lowercase();
    let stem = name.trim_end_matches(".exe");
    candidates.iter().any(|candidate| {
        let candidate = candidate.to_ascii_lowercase();
        let candidate_stem = candidate.trim_end_matches(".exe");
        candidate == name || candidate_stem == stem
    })
}

fn known_aliases() -> HashMap<&'static str, &'static [&'static str]> {
    HashMap::from([
        ("counter-strike 2", &["cs2.exe"] as &[_]),
        ("roblox", &["robloxplayerbeta.exe"] as &[_]),
        ("fortnite", &["fortniteclient-win64-shipping.exe"] as &[_]),
        ("valorant", &["valorant-win64-shipping.exe"] as &[_]),
        (
            "league of legends",
            &["leagueclientuxrender.exe", "league of legends.exe"] as &[_],
        ),
        (
            "dead by daylight",
            &["deadbydaylight-win64-shipping.exe"] as &[_],
        ),
        ("minecraft", &["minecraft.windows.exe", "javaw.exe"] as &[_]),
    ])
}

fn path_text_key(value: &str) -> String {
    value.replace('/', "\\").to_ascii_lowercase()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_game_alias_matches_the_actual_executable() {
        let candidates = candidate_names(["Counter-Strike 2"]);
        assert!(candidate_matches("cs2.exe", &candidates));
    }

    #[test]
    fn generic_labels_do_not_become_file_lookup_candidates() {
        assert!(candidate_names(["System audio", "Microphone"]).is_empty());
    }
}
