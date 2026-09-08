use std::{
    collections::HashSet,
    fs,
    path::{Path, PathBuf},
    sync::Arc,
};

use crate::{contracts::ClipRecord, error::AppResult};

use super::{is_supported_video, ClipRepository};

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ClipMutation {
    pub changed: bool,
    pub removed_paths: Vec<PathBuf>,
    pub failed_paths: Vec<PathBuf>,
}

pub struct ClipService {
    repository: Arc<ClipRepository>,
}

impl ClipService {
    pub fn new(repository: Arc<ClipRepository>) -> Self {
        Self { repository }
    }

    pub fn repository(&self) -> &Arc<ClipRepository> {
        &self.repository
    }

    /// Mirrors Electron's list cleanup and enriches legacy records without
    /// making the renderer infer folder ownership.
    pub fn list_saved(&self, save_folder: &Path) -> AppResult<Vec<ClipRecord>> {
        let mut removed_stale = false;
        let mut records = self.repository.load()?;
        records.retain(|record| {
            let exists = Path::new(&record.file_path).is_file();
            removed_stale |= !exists;
            exists
        });
        if removed_stale {
            self.repository.replace(&records)?;
        }
        for record in &mut records {
            record.library_source = Some("clip".into());
            if record.folder_name.as_deref().is_none_or(str::is_empty) {
                record.folder_name = Some(folder_name(&record.file_path, save_folder));
            }
        }
        Ok(records)
    }

    pub fn append_saved(&self, mut record: ClipRecord) -> AppResult<()> {
        record.library_source = Some("clip".into());
        self.repository.append(record)
    }

    pub fn delete_saved(&self, ids: &[String]) -> AppResult<ClipMutation> {
        let wanted: HashSet<_> = ids
            .iter()
            .filter(|id| !id.starts_with("imported:"))
            .collect();
        if wanted.is_empty() {
            return Ok(ClipMutation::default());
        }
        self.repository.update(|records| {
            let mut mutation = ClipMutation::default();
            records.retain(|record| {
                if !wanted.contains(&record.id) {
                    return true;
                }
                mutation.changed = true;
                for path in deletable_record_files(record) {
                    match fs::remove_file(&path) {
                        Ok(()) => mutation.removed_paths.push(path),
                        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                        Err(_) => mutation.failed_paths.push(path),
                    }
                }
                false
            });
            Ok(mutation)
        })
    }

    pub fn rename_saved(&self, id: &str, requested_title: &str) -> AppResult<bool> {
        let title = requested_title.trim();
        if title.is_empty() || id.starts_with("imported:") {
            return Ok(false);
        }
        self.repository.update(|records| {
            let Some(record) = records.iter_mut().find(|record| record.id == id) else {
                return Ok(false);
            };
            record.title = title.to_owned();
            let old_path = PathBuf::from(&record.file_path);
            if safe_regular_video(&old_path) {
                let extension = old_path
                    .extension()
                    .and_then(|value| value.to_str())
                    .unwrap_or("mp4");
                let stem = safe_file_stem(title, "clip");
                let parent = old_path.parent().unwrap_or_else(|| Path::new("."));
                let new_path = unique_destination(parent, &stem, extension, &old_path);
                if path_key(&new_path) != path_key(&old_path)
                    && fs::rename(&old_path, &new_path).is_ok()
                {
                    record.file_path = new_path.to_string_lossy().into_owned();
                }
            }
            Ok(true)
        })
    }
}

fn deletable_record_files(record: &ClipRecord) -> Vec<PathBuf> {
    let primary = PathBuf::from(&record.file_path);
    if !safe_regular_video(&primary) {
        return Vec::new();
    }
    let parent = primary.parent().map(Path::to_owned);
    std::iter::once(primary)
        .chain(
            record
                .segment_files
                .clone()
                .unwrap_or_default()
                .into_iter()
                .map(PathBuf::from),
        )
        .filter(|path| is_supported_video(path) && path.parent() == parent.as_deref())
        .collect()
}

fn safe_regular_video(path: &Path) -> bool {
    if !is_supported_video(path) {
        return false;
    }
    fs::symlink_metadata(path)
        .is_ok_and(|metadata| metadata.file_type().is_file() && !metadata.file_type().is_symlink())
}

pub fn safe_file_stem(value: &str, fallback: &str) -> String {
    let sanitized: String = value
        .trim()
        .chars()
        .map(|character| match character {
            '\\' | '/' | ':' | '*' | '?' | '"' | '<' | '>' | '|' | '\0'..='\u{1f}' => '_',
            value => value,
        })
        .collect();
    let sanitized = sanitized.trim().trim_end_matches(['.', ' ']);
    if sanitized.is_empty() {
        fallback.into()
    } else {
        sanitized.chars().take(180).collect()
    }
}

pub fn unique_destination(parent: &Path, stem: &str, extension: &str, current: &Path) -> PathBuf {
    let extension = extension.trim_start_matches('.');
    let mut counter = 0_u32;
    loop {
        let suffix = if counter == 0 {
            String::new()
        } else {
            format!("_{counter}")
        };
        let candidate = parent.join(format!("{stem}{suffix}.{extension}"));
        if !candidate.exists() || path_key(&candidate) == path_key(current) {
            return candidate;
        }
        counter = counter.saturating_add(1);
    }
}

fn folder_name(file_path: &str, save_folder: &Path) -> String {
    let parent = Path::new(file_path)
        .parent()
        .and_then(Path::file_name)
        .and_then(|value| value.to_str())
        .unwrap_or("");
    let save_name = save_folder
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("");
    if parent.is_empty() || parent.eq_ignore_ascii_case(save_name) {
        "Clips".into()
    } else {
        parent.into()
    }
}

pub(crate) fn path_key(path: &Path) -> String {
    let value = path.to_string_lossy().replace('/', "\\");
    #[cfg(windows)]
    return value.to_ascii_lowercase();
    #[cfg(not(windows))]
    value
}

#[cfg(test)]
mod tests {
    use super::*;

    fn record(id: &str, path: &Path) -> ClipRecord {
        ClipRecord {
            id: id.into(),
            title: "Before".into(),
            file_path: path.to_string_lossy().into_owned(),
            ..ClipRecord::default()
        }
    }

    #[test]
    fn mutations_only_touch_files_selected_by_record_id() {
        let root = tempfile::tempdir().unwrap();
        let clip_path = root.path().join("clip.mp4");
        let unrelated = root.path().join("unrelated.mp4");
        fs::write(&clip_path, b"clip").unwrap();
        fs::write(&unrelated, b"keep").unwrap();
        let repository = Arc::new(ClipRepository::new(root.path().join("clips.json")));
        repository.append(record("known", &clip_path)).unwrap();
        let service = ClipService::new(repository);

        assert!(!service.delete_saved(&["unknown".into()]).unwrap().changed);
        assert!(unrelated.exists());
        assert!(service.delete_saved(&["known".into()]).unwrap().changed);
        assert!(!clip_path.exists());
        assert!(unrelated.exists());
    }

    #[test]
    fn rename_sanitizes_the_filename_but_preserves_the_display_title() {
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("before.mp4");
        fs::write(&path, b"clip").unwrap();
        let repository = Arc::new(ClipRepository::new(root.path().join("clips.json")));
        repository.append(record("known", &path)).unwrap();
        let service = ClipService::new(repository.clone());

        assert!(service.rename_saved("known", "After: One").unwrap());
        let record = repository.load().unwrap().pop().unwrap();
        assert_eq!(record.title, "After: One");
        assert!(record.file_path.ends_with("After_ One.mp4"));
    }

    #[test]
    fn stale_records_are_pruned_only_inside_the_fixture_repository() {
        let root = tempfile::tempdir().unwrap();
        let repository = Arc::new(ClipRepository::new(root.path().join("clips.json")));
        repository
            .append(record("missing", &root.path().join("gone.mp4")))
            .unwrap();
        let service = ClipService::new(repository.clone());
        assert!(service
            .list_saved(&root.path().join("Clips"))
            .unwrap()
            .is_empty());
        assert!(repository.load().unwrap().is_empty());
    }
}
