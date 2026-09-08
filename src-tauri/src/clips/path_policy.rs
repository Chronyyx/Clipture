use std::{
    collections::HashMap,
    fs,
    path::{Path, PathBuf},
};

use crate::contracts::ClipRecord;

#[derive(Clone, Debug)]
pub struct AuthorizedClip {
    pub id: String,
    pub primary: PathBuf,
    pub segments: Vec<PathBuf>,
    pub imported: bool,
}

/// A point-in-time allow-list built only from repository or imported-index
/// records. Renderer-provided paths are deliberately absent from this API.
#[derive(Clone, Debug, Default)]
pub struct PathAuthorizer {
    clips: HashMap<String, AuthorizedClip>,
}

impl PathAuthorizer {
    pub fn from_records(records: impl IntoIterator<Item = ClipRecord>) -> Self {
        let mut clips = HashMap::new();
        for record in records {
            let primary = match canonical_video(&record.file_path) {
                Some(path) => path,
                None => continue,
            };
            let parent = primary.parent().map(Path::to_owned);
            let segments = record
                .segment_files
                .unwrap_or_default()
                .into_iter()
                .filter_map(|path| canonical_video(path))
                .filter(|path| path.parent() == parent.as_deref())
                .collect();
            clips.insert(
                record.id.clone(),
                AuthorizedClip {
                    id: record.id,
                    primary,
                    segments,
                    imported: record.library_source.as_deref() == Some("imported"),
                },
            );
        }
        Self { clips }
    }

    pub fn clip(&self, id: &str) -> Option<&AuthorizedClip> {
        self.clips.get(id)
    }

    pub fn primary(&self, id: &str) -> Option<&Path> {
        self.clip(id).map(|clip| clip.primary.as_path())
    }

    /// Resolves the temporary v1 renderer contract's file-path argument back
    /// to an application-owned clip identifier. The candidate path is never
    /// returned or opened directly; it must match a canonical path captured
    /// from the current library snapshot.
    pub fn clip_id_for_path(&self, path: &Path) -> Option<&str> {
        let canonical = path.canonicalize().ok()?;
        self.clips
            .values()
            .find(|clip| clip.primary == canonical)
            .map(|clip| clip.id.as_str())
    }

    pub fn contains_canonical_path(&self, path: &Path) -> bool {
        let Ok(path) = path.canonicalize() else {
            return false;
        };
        self.clips.values().any(|clip| {
            clip.primary == path || clip.segments.iter().any(|segment| segment == &path)
        })
    }
}

pub fn is_supported_video(path: &Path) -> bool {
    matches!(
        path.extension()
            .and_then(|extension| extension.to_str())
            .map(str::to_ascii_lowercase)
            .as_deref(),
        Some("mp4" | "m4v" | "mov" | "webm" | "mkv" | "avi")
    )
}

fn canonical_video(path: impl AsRef<Path>) -> Option<PathBuf> {
    let path = path.as_ref();
    if !is_supported_video(path) {
        return None;
    }
    let metadata = fs::symlink_metadata(path).ok()?;
    if !metadata.file_type().is_file() || metadata.file_type().is_symlink() {
        return None;
    }
    path.canonicalize().ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_recorded_existing_video_files_are_authorized() {
        let root = tempfile::tempdir().unwrap();
        let video = root.path().join("clip.mp4");
        let text = root.path().join("secret.txt");
        fs::write(&video, b"video").unwrap();
        fs::write(&text, b"secret").unwrap();
        let authority = PathAuthorizer::from_records([
            ClipRecord {
                id: "video".into(),
                file_path: video.to_string_lossy().into(),
                ..ClipRecord::default()
            },
            ClipRecord {
                id: "text".into(),
                file_path: text.to_string_lossy().into(),
                ..ClipRecord::default()
            },
        ]);

        assert!(authority.primary("video").is_some());
        assert_eq!(authority.clip_id_for_path(&video), Some("video"));
        assert!(authority.primary("text").is_none());
        assert_eq!(authority.clip_id_for_path(&text), None);
        assert!(!authority.contains_canonical_path(&text));
    }

    #[test]
    fn reverse_lookup_rejects_an_existing_unrecorded_video() {
        let root = tempfile::tempdir().unwrap();
        let recorded = root.path().join("recorded.mp4");
        let unrelated = root.path().join("unrelated.mp4");
        fs::write(&recorded, b"video").unwrap();
        fs::write(&unrelated, b"private").unwrap();
        let authority = PathAuthorizer::from_records([ClipRecord {
            id: "recorded".into(),
            file_path: recorded.to_string_lossy().into(),
            ..ClipRecord::default()
        }]);

        assert_eq!(authority.clip_id_for_path(&unrelated), None);
    }
}
