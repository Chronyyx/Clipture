use std::{
    collections::{BTreeMap, HashSet},
    fs,
    path::{Path, PathBuf},
    sync::{Arc, RwLock},
};

use crate::{
    clips::{safe_file_stem, unique_destination, ClipService, PathAuthorizer},
    contracts::{ClipRecord, ClipSettings},
    error::AppResult,
};

use super::{scanner::normalized_path_key, ImportedScanner, ScanReport};

#[derive(Clone, Debug, Default)]
pub struct ImportedMutation {
    pub changed: bool,
    pub settings: Option<ClipSettings>,
    pub removed_paths: Vec<PathBuf>,
    pub failed_paths: Vec<PathBuf>,
    pub renamed_path: Option<PathBuf>,
}

#[derive(Clone, Debug, Default)]
struct ImportedIndex {
    roots_key: String,
    clips: Vec<ClipRecord>,
    report: ScanReport,
}

pub struct LibraryService {
    saved: Arc<ClipService>,
    scanner: ImportedScanner,
    imported: RwLock<ImportedIndex>,
}

impl LibraryService {
    pub fn new(saved: Arc<ClipService>, scanner: ImportedScanner) -> Self {
        Self {
            saved,
            scanner,
            imported: RwLock::new(ImportedIndex::default()),
        }
    }

    pub fn refresh_imported(&self, settings: &ClipSettings) -> AppResult<ScanReport> {
        let report = self.scanner.scan(settings)?;
        *self
            .imported
            .write()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = ImportedIndex {
            roots_key: roots_key(settings),
            clips: report.clips.clone(),
            report: report.clone(),
        };
        Ok(report)
    }

    /// Returns quickly when the imported index is warm. The caller should run
    /// `refresh_imported` on a blocking/background worker when this returns no
    /// imported records for a newly changed roots key.
    pub fn list_cached(&self, settings: &ClipSettings) -> AppResult<Vec<ClipRecord>> {
        let mut clips = self.saved.list_saved(Path::new(&settings.save_folder))?;
        let imported = self
            .imported
            .read()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if imported.roots_key == roots_key(settings) {
            clips.extend(imported.clips.clone());
        }
        Ok(clips)
    }

    pub fn list_refreshed(&self, settings: &ClipSettings) -> AppResult<Vec<ClipRecord>> {
        if self
            .imported
            .read()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .roots_key
            != roots_key(settings)
        {
            self.refresh_imported(settings)?;
        }
        self.list_cached(settings)
    }

    pub fn latest_scan_report(&self) -> ScanReport {
        self.imported
            .read()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .report
            .clone()
    }

    pub fn path_authorizer(&self, settings: &ClipSettings) -> AppResult<PathAuthorizer> {
        Ok(PathAuthorizer::from_records(self.list_refreshed(settings)?))
    }

    pub fn add_import_roots(
        &self,
        settings: &ClipSettings,
        selected_roots: &[PathBuf],
    ) -> AppResult<ImportedMutation> {
        let mut next = settings.clone();
        let mut seen: HashSet<_> = next
            .imported_video_directories
            .iter()
            .map(|path| normalized_path_key(Path::new(path)))
            .collect();
        let before = next.imported_video_directories.len();
        for root in selected_roots {
            if !safe_directory(root) {
                continue;
            }
            let normalized = root.to_string_lossy().into_owned();
            if seen.insert(normalized_path_key(root)) {
                next.imported_video_directories.push(normalized);
            }
        }
        let changed = next.imported_video_directories.len() != before;
        if changed {
            self.refresh_imported(&next)?;
        }
        Ok(ImportedMutation {
            changed,
            settings: changed.then_some(next),
            ..ImportedMutation::default()
        })
    }

    pub fn rename_imported(
        &self,
        settings: &ClipSettings,
        id: &str,
        requested_title: &str,
    ) -> AppResult<ImportedMutation> {
        let title = requested_title.trim();
        if title.is_empty() || !id.starts_with("imported:") {
            return Ok(ImportedMutation::default());
        }
        let records = self.refresh_imported(settings)?.clips;
        let Some(record) = records.into_iter().find(|record| record.id == id) else {
            return Ok(ImportedMutation::default());
        };
        let old_path = PathBuf::from(&record.file_path);
        if !safe_regular_file(&old_path) {
            return Ok(ImportedMutation::default());
        }
        let extension = old_path
            .extension()
            .and_then(|value| value.to_str())
            .unwrap_or("mp4");
        let parent = old_path.parent().unwrap_or_else(|| Path::new("."));
        let stem = safe_file_stem(title, "video");
        let new_path = unique_destination(parent, &stem, extension, &old_path);
        if normalized_path_key(&new_path) != normalized_path_key(&old_path) {
            if fs::rename(&old_path, &new_path).is_err() {
                return Ok(ImportedMutation::default());
            }
        }

        let mut next = settings.clone();
        remove_title_override(&mut next.imported_video_titles, &old_path);
        next.imported_video_titles
            .insert(new_path.to_string_lossy().into_owned(), title.to_owned());
        self.refresh_imported(&next)?;
        Ok(ImportedMutation {
            changed: true,
            settings: Some(next),
            renamed_path: Some(new_path),
            ..ImportedMutation::default()
        })
    }

    pub fn delete_imported(
        &self,
        settings: &ClipSettings,
        ids: &[String],
    ) -> AppResult<ImportedMutation> {
        let wanted: HashSet<_> = ids
            .iter()
            .filter(|id| id.starts_with("imported:"))
            .collect();
        if wanted.is_empty() {
            return Ok(ImportedMutation::default());
        }
        let records = self.refresh_imported(settings)?.clips;
        let mut next = settings.clone();
        let mut mutation = ImportedMutation::default();
        for record in records {
            if !wanted.contains(&record.id) {
                continue;
            }
            let path = PathBuf::from(&record.file_path);
            if !safe_regular_file(&path) {
                continue;
            }
            match fs::remove_file(&path) {
                Ok(()) => {
                    mutation.changed = true;
                    mutation.removed_paths.push(path.clone());
                    remove_title_override(&mut next.imported_video_titles, &path);
                }
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                Err(_) => mutation.failed_paths.push(path),
            }
        }
        if mutation.changed {
            self.refresh_imported(&next)?;
            mutation.settings = Some(next);
        }
        Ok(mutation)
    }
}

fn roots_key(settings: &ClipSettings) -> String {
    settings
        .imported_video_directories
        .iter()
        .map(|path| normalized_path_key(Path::new(path)))
        .collect::<Vec<_>>()
        .join("\n")
}

fn remove_title_override(titles: &mut BTreeMap<String, String>, path: &Path) {
    let key = normalized_path_key(path);
    titles.retain(|candidate, _| normalized_path_key(Path::new(candidate)) != key);
}

fn safe_directory(path: &Path) -> bool {
    fs::symlink_metadata(path)
        .is_ok_and(|metadata| metadata.file_type().is_dir() && !metadata.file_type().is_symlink())
}

fn safe_regular_file(path: &Path) -> bool {
    crate::clips::is_supported_video(path)
        && fs::symlink_metadata(path).is_ok_and(|metadata| {
            metadata.file_type().is_file() && !metadata.file_type().is_symlink()
        })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::clips::ClipRepository;

    fn service(root: &Path) -> LibraryService {
        let repository = Arc::new(ClipRepository::new(root.join("data/clips.json")));
        LibraryService::new(
            Arc::new(ClipService::new(repository)),
            ImportedScanner::default(),
        )
    }

    #[test]
    fn imported_mutations_cannot_target_an_unindexed_path() {
        let root = tempfile::tempdir().unwrap();
        let imported = root.path().join("imported");
        fs::create_dir(&imported).unwrap();
        let indexed = imported.join("indexed.mp4");
        let outside = root.path().join("outside.mp4");
        fs::write(&indexed, b"video").unwrap();
        fs::write(&outside, b"keep").unwrap();
        let mut settings = ClipSettings::default();
        settings.imported_video_directories = vec![imported.to_string_lossy().into()];
        let service = service(root.path());

        let mutation = service
            .delete_imported(&settings, &["imported:not-a-real-id".into()])
            .unwrap();
        assert!(!mutation.changed);
        assert!(indexed.exists());
        assert!(outside.exists());
    }

    #[test]
    fn imported_rename_updates_path_keyed_title_override() {
        let root = tempfile::tempdir().unwrap();
        let imported = root.path().join("imported");
        fs::create_dir(&imported).unwrap();
        let video = imported.join("before.mp4");
        fs::write(&video, b"video").unwrap();
        let mut settings = ClipSettings::default();
        settings.imported_video_directories = vec![imported.to_string_lossy().into()];
        let service = service(root.path());
        let id = service.refresh_imported(&settings).unwrap().clips[0]
            .id
            .clone();

        let mutation = service
            .rename_imported(&settings, &id, "After: Clip")
            .unwrap();
        assert!(mutation.changed);
        assert!(mutation.renamed_path.unwrap().ends_with("After_ Clip.mp4"));
        assert_eq!(
            mutation
                .settings
                .unwrap()
                .imported_video_titles
                .values()
                .next()
                .unwrap(),
            "After: Clip"
        );
    }
}
