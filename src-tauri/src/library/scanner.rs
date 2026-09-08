use std::{
    collections::{BTreeMap, HashSet, VecDeque},
    fs,
    path::{Path, PathBuf},
    sync::Arc,
    thread,
};

use serde::Serialize;

use crate::{
    contracts::{ClipRecord, ClipSettings},
    error::AppResult,
};

use super::{sha1::digest_hex, timestamp::iso_utc};

pub trait ScanPacer: Send + Sync {
    fn checkpoint(&self, visited_entries: usize);
}

#[derive(Default)]
pub struct YieldingScanPacer;

impl ScanPacer for YieldingScanPacer {
    fn checkpoint(&self, visited_entries: usize) {
        if visited_entries % 128 == 0 {
            thread::yield_now();
        }
    }
}

#[derive(Clone, Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanReport {
    pub clips: Vec<ClipRecord>,
    pub scanned_entries: usize,
    pub truncated_roots: Vec<String>,
    pub skipped_roots: Vec<String>,
}

pub struct ImportedScanner {
    max_entries_per_root: usize,
    pacer: Arc<dyn ScanPacer>,
}

impl Default for ImportedScanner {
    fn default() -> Self {
        Self::new(6_000, Arc::new(YieldingScanPacer))
    }
}

impl ImportedScanner {
    pub fn new(max_entries_per_root: usize, pacer: Arc<dyn ScanPacer>) -> Self {
        Self {
            max_entries_per_root: max_entries_per_root.max(1),
            pacer,
        }
    }

    pub fn scan(&self, settings: &ClipSettings) -> AppResult<ScanReport> {
        let title_overrides: BTreeMap<_, _> = settings
            .imported_video_titles
            .iter()
            .map(|(path, title)| {
                (
                    normalized_path_key(Path::new(path)),
                    title.trim().to_owned(),
                )
            })
            .collect();
        let mut report = ScanReport::default();
        let mut seen_roots = HashSet::new();

        for configured_root in &settings.imported_video_directories {
            let root = PathBuf::from(configured_root);
            let root_key = normalized_path_key(&root);
            if root_key.is_empty() || !seen_roots.insert(root_key) || !safe_directory(&root) {
                report.skipped_roots.push(configured_root.clone());
                continue;
            }
            self.scan_root(&root, &title_overrides, &mut report);
        }
        report.clips.sort_by(|left, right| {
            right
                .created_at
                .cmp(&left.created_at)
                .then_with(|| left.file_path.cmp(&right.file_path))
        });
        Ok(report)
    }

    fn scan_root(
        &self,
        root: &Path,
        title_overrides: &BTreeMap<String, String>,
        report: &mut ScanReport,
    ) {
        let folder_name = root
            .file_name()
            .and_then(|value| value.to_str())
            .filter(|value| !value.is_empty())
            .unwrap_or("Imported videos")
            .to_owned();
        let mut queue = VecDeque::from([root.to_owned()]);
        let mut scanned = 0_usize;
        let mut truncated = false;

        while let Some(directory) = queue.pop_front() {
            if scanned >= self.max_entries_per_root {
                truncated = true;
                break;
            }
            self.pacer.checkpoint(scanned);
            let Ok(entries) = fs::read_dir(&directory) else {
                continue;
            };
            for entry in entries.flatten() {
                if scanned >= self.max_entries_per_root {
                    truncated = true;
                    break;
                }
                scanned += 1;
                report.scanned_entries += 1;
                self.pacer.checkpoint(scanned);
                let path = entry.path();
                let Ok(file_type) = entry.file_type() else {
                    continue;
                };
                if file_type.is_symlink() {
                    continue;
                }
                if file_type.is_dir() {
                    let hidden = entry.file_name().to_string_lossy().starts_with('.');
                    if !hidden {
                        queue.push_back(path);
                    }
                    continue;
                }
                if !file_type.is_file() || !crate::clips::is_supported_video(&path) {
                    continue;
                }
                let Ok(metadata) = entry.metadata() else {
                    continue;
                };
                let path_text = path.to_string_lossy().into_owned();
                let path_key = normalized_path_key(&path);
                let default_title = path
                    .file_stem()
                    .and_then(|value| value.to_str())
                    .filter(|value| !value.is_empty())
                    .unwrap_or("Imported video");
                report.clips.push(ClipRecord {
                    id: imported_clip_id(&path),
                    title: title_overrides
                        .get(&path_key)
                        .filter(|title| !title.is_empty())
                        .cloned()
                        .unwrap_or_else(|| default_title.into()),
                    game_or_app: folder_name.clone(),
                    library_source: Some("imported".into()),
                    folder_name: Some(folder_name.clone()),
                    imported_root: Some(root.to_string_lossy().into_owned()),
                    created_at: iso_utc(metadata.modified().unwrap_or(std::time::UNIX_EPOCH)),
                    file_path: path_text,
                    resolution: "Imported".into(),
                    encoder: "Imported video".into(),
                    audio_tracks: vec!["Imported audio".into()],
                    ..ClipRecord::default()
                });
            }
            if truncated {
                break;
            }
        }

        if truncated {
            report
                .truncated_roots
                .push(root.to_string_lossy().into_owned());
        }
    }
}

pub(crate) fn imported_clip_id(path: &Path) -> String {
    format!(
        "imported:{}",
        digest_hex(normalized_path_key(path).as_bytes())
    )
}

pub(crate) fn normalized_path_key(path: &Path) -> String {
    path.to_string_lossy()
        .replace('/', "\\")
        .to_ascii_lowercase()
}

fn safe_directory(path: &Path) -> bool {
    fs::symlink_metadata(path)
        .is_ok_and(|metadata| metadata.file_type().is_dir() && !metadata.file_type().is_symlink())
}

#[cfg(test)]
mod tests {
    use super::*;

    struct NoopPacer;
    impl ScanPacer for NoopPacer {
        fn checkpoint(&self, _: usize) {}
    }

    #[test]
    fn scans_supported_files_without_following_hidden_directories() {
        let root = tempfile::tempdir().unwrap();
        fs::create_dir(root.path().join("nested")).unwrap();
        fs::create_dir(root.path().join(".hidden")).unwrap();
        fs::write(root.path().join("nested/clip.mp4"), b"video").unwrap();
        fs::write(root.path().join("nested/readme.txt"), b"text").unwrap();
        fs::write(root.path().join(".hidden/private.mp4"), b"video").unwrap();
        let mut settings = ClipSettings::default();
        settings.imported_video_directories = vec![root.path().to_string_lossy().into()];
        let scanner = ImportedScanner::new(100, Arc::new(NoopPacer));

        let report = scanner.scan(&settings).unwrap();
        assert_eq!(report.clips.len(), 1);
        assert_eq!(report.clips[0].title, "clip");
        assert!(report.clips[0].id.starts_with("imported:"));
    }

    #[test]
    fn entry_limit_is_per_root_and_reported() {
        let root = tempfile::tempdir().unwrap();
        for index in 0..5 {
            fs::write(root.path().join(format!("{index}.mp4")), b"video").unwrap();
        }
        let mut settings = ClipSettings::default();
        settings.imported_video_directories = vec![root.path().to_string_lossy().into()];
        let report = ImportedScanner::new(2, Arc::new(NoopPacer))
            .scan(&settings)
            .unwrap();
        assert!(report.clips.len() <= 2);
        assert_eq!(report.truncated_roots.len(), 1);
    }
}
