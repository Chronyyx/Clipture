use std::{
    fs,
    path::{Path, PathBuf},
};

use serde::Serialize;

use crate::error::{AppError, AppResult};

#[derive(Clone, Debug)]
pub struct BundledSound {
    pub file_name: String,
    pub label: String,
    pub source: PathBuf,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ClipSoundOption {
    pub id: String,
    pub label: String,
    pub url: String,
    pub built_in: bool,
}

pub struct SoundLibrary {
    sounds_dir: PathBuf,
    bundled: Vec<BundledSound>,
}

impl SoundLibrary {
    pub fn new(sounds_dir: impl Into<PathBuf>, bundled: Vec<BundledSound>) -> Self {
        Self {
            sounds_dir: sounds_dir.into(),
            bundled,
        }
    }

    pub fn sounds_dir(&self) -> &Path {
        &self.sounds_dir
    }

    pub fn list(&self) -> AppResult<Vec<ClipSoundOption>> {
        self.ensure_bundled()?;
        let mut sounds = Vec::new();
        let entries = fs::read_dir(&self.sounds_dir).map_err(|source| AppError::Io {
            action: "list clip sounds",
            path: self.sounds_dir.clone(),
            source,
        })?;
        for entry in entries.flatten() {
            let Ok(file_type) = entry.file_type() else {
                continue;
            };
            if !file_type.is_file() || file_type.is_symlink() || !supported_audio(&entry.path()) {
                continue;
            }
            let file_name = entry.file_name().to_string_lossy().into_owned();
            let built_in = self
                .bundled
                .iter()
                .find(|sound| sound.file_name.eq_ignore_ascii_case(&file_name));
            sounds.push(ClipSoundOption {
                id: built_in
                    .map(|_| file_name.clone())
                    .unwrap_or_else(|| format!("custom:{file_name}")),
                label: built_in
                    .map(|sound| sound.label.clone())
                    .unwrap_or_else(|| label_from_file_name(&file_name)),
                // The Tauri adapter converts this host-authorized path to its
                // asset URL for UI-only previews. Background playback is native.
                url: entry.path().to_string_lossy().into_owned(),
                built_in: built_in.is_some(),
            });
        }
        sounds.sort_by(|left, right| {
            left.label
                .to_ascii_lowercase()
                .cmp(&right.label.to_ascii_lowercase())
        });
        Ok(sounds)
    }

    pub fn import(&self, source: &Path) -> AppResult<ClipSoundOption> {
        if !supported_audio(source)
            || !fs::symlink_metadata(source).is_ok_and(|metadata| {
                metadata.file_type().is_file() && !metadata.file_type().is_symlink()
            })
        {
            return Err(AppError::Path(
                "selected sound must be an mp3, wav, or ogg file".into(),
            ));
        }
        fs::create_dir_all(&self.sounds_dir).map_err(|source| AppError::Io {
            action: "create clip sound directory",
            path: self.sounds_dir.clone(),
            source,
        })?;
        let destination = self.unique_destination(source);
        fs::copy(source, &destination).map_err(|source| AppError::Io {
            action: "import clip sound",
            path: destination.clone(),
            source,
        })?;
        let file_name = destination
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned();
        Ok(ClipSoundOption {
            id: format!("custom:{file_name}"),
            label: label_from_file_name(&file_name),
            url: destination.to_string_lossy().into_owned(),
            built_in: false,
        })
    }

    pub fn resolve(&self, id: &str) -> AppResult<Option<PathBuf>> {
        if id == "none" || id.trim().is_empty() {
            return Ok(None);
        }
        let file_name = id.strip_prefix("custom:").unwrap_or(id);
        if Path::new(file_name)
            .file_name()
            .and_then(|value| value.to_str())
            != Some(file_name)
        {
            return Err(AppError::Path("sound identifier contains a path".into()));
        }
        let path = self.sounds_dir.join(file_name);
        if !supported_audio(&path) {
            return Err(AppError::Path(
                "sound identifier has an unsupported extension".into(),
            ));
        }
        // Tray-first launches may never enumerate sounds in a WebView.
        // Install defaults here as well as in list(), before resolving playback.
        self.ensure_bundled()?;
        let metadata = match fs::symlink_metadata(&path) {
            Ok(metadata) => metadata,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(source) => {
                return Err(AppError::Io {
                    action: "inspect clip sound",
                    path,
                    source,
                })
            }
        };
        if !metadata.file_type().is_file() || metadata.file_type().is_symlink() {
            return Ok(None);
        }
        Ok(Some(path))
    }

    fn ensure_bundled(&self) -> AppResult<()> {
        fs::create_dir_all(&self.sounds_dir).map_err(|source| AppError::Io {
            action: "create clip sound directory",
            path: self.sounds_dir.clone(),
            source,
        })?;
        for sound in &self.bundled {
            let destination = self.sounds_dir.join(&sound.file_name);
            if destination.exists() || !sound.source.is_file() {
                continue;
            }
            fs::copy(&sound.source, &destination).map_err(|source| AppError::Io {
                action: "install bundled clip sound",
                path: destination,
                source,
            })?;
        }
        Ok(())
    }

    fn unique_destination(&self, source: &Path) -> PathBuf {
        let safe_name = safe_sound_file_name(source);
        let parsed = Path::new(&safe_name);
        let stem = parsed
            .file_stem()
            .and_then(|value| value.to_str())
            .unwrap_or("sound");
        let extension = parsed
            .extension()
            .and_then(|value| value.to_str())
            .unwrap_or("wav");
        let mut counter = 0_u32;
        loop {
            let suffix = if counter == 0 {
                String::new()
            } else {
                format!("-{counter}")
            };
            let candidate = self.sounds_dir.join(format!("{stem}{suffix}.{extension}"));
            if !candidate.exists() {
                return candidate;
            }
            counter = counter.saturating_add(1);
        }
    }
}

fn supported_audio(path: &Path) -> bool {
    matches!(
        path.extension()
            .and_then(|value| value.to_str())
            .map(str::to_ascii_lowercase)
            .as_deref(),
        Some("mp3" | "wav" | "ogg")
    )
}

fn safe_sound_file_name(path: &Path) -> String {
    let stem: String = path
        .file_stem()
        .and_then(|value| value.to_str())
        .unwrap_or("sound")
        .chars()
        .filter(|character| {
            character.is_ascii_alphanumeric() || matches!(character, '.' | '_' | '-' | ' ')
        })
        .take(80)
        .collect();
    let stem = stem.split_whitespace().collect::<Vec<_>>().join(" ");
    let extension = path
        .extension()
        .and_then(|value| value.to_str())
        .unwrap_or("wav")
        .to_ascii_lowercase();
    format!(
        "{}.{}",
        if stem.is_empty() { "sound" } else { &stem },
        extension
    )
}

fn label_from_file_name(file_name: &str) -> String {
    Path::new(file_name)
        .file_stem()
        .and_then(|value| value.to_str())
        .unwrap_or("sound")
        .replace(['-', '_'], " ")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn imports_unique_sanitized_names_inside_injected_directory() {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("source.mp3");
        fs::write(&source, b"audio").unwrap();
        let library = SoundLibrary::new(root.path().join("sounds"), vec![]);
        let first = library.import(&source).unwrap();
        let second = library.import(&source).unwrap();
        assert_ne!(first.id, second.id);
        assert_eq!(library.list().unwrap().len(), 2);
    }

    #[test]
    fn bundled_sound_resolves_before_the_library_is_ever_listed() {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("bundled.wav");
        fs::write(&source, b"default audio").unwrap();
        let library = SoundLibrary::new(
            root.path().join("sounds"),
            vec![BundledSound {
                file_name: "clip.wav".into(),
                label: "Clip".into(),
                source,
            }],
        );
        let installed = library.resolve("clip.wav").unwrap().unwrap();
        assert_eq!(fs::read(&installed).unwrap(), b"default audio");
        fs::write(&installed, b"existing user sound").unwrap();
        assert_eq!(
            library.resolve("clip.wav").unwrap(),
            Some(installed.clone())
        );
        assert_eq!(fs::read(installed).unwrap(), b"existing user sound");
    }

    #[test]
    fn identifier_cannot_escape_the_sound_directory() {
        let root = tempfile::tempdir().unwrap();
        let library = SoundLibrary::new(root.path().join("sounds"), vec![]);
        assert!(library.resolve("custom:../secret.wav").is_err());
    }
}
