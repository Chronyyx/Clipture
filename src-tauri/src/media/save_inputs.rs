use std::{
    collections::HashSet,
    fs,
    path::{Path, PathBuf},
};

use crate::{
    contracts::{ClipRecord, ClipSettings},
    error::{AppError, AppResult},
};

pub struct SaveInputs {
    pub root: PathBuf,
    pub files: Vec<PathBuf>,
    pub segmented: bool,
}

impl SaveInputs {
    pub fn validate(clip: &ClipRecord, settings: &ClipSettings) -> AppResult<Self> {
        if clip.id.trim().is_empty() || clip.audio_tracks.len() > 16 {
            return Err(AppError::Path(
                "Saved clip identity or track count is invalid".into(),
            ));
        }
        let root = Path::new(&settings.save_folder)
            .canonicalize()
            .map_err(|error| io_error(&settings.save_folder, error))?;
        let output = Path::new(&clip.file_path);
        validate_location(output, &root)?;
        let segments = clip.segment_files.as_deref().unwrap_or_default();
        if segments.len() > 32 {
            return Err(AppError::Path(
                "Clip exceeds the 32-segment processing limit; original segments preserved".into(),
            ));
        }
        let segmented = !segments.is_empty();
        let paths = if segmented {
            segments.to_vec()
        } else {
            vec![clip.file_path.clone()]
        };
        let mut seen = HashSet::new();
        let mut files = Vec::new();
        for value in paths {
            let path = Path::new(&value);
            validate_location(path, &root)?;
            let metadata = fs::symlink_metadata(path).map_err(|error| io_error(&value, error))?;
            if !metadata.is_file() || metadata.len() < 32 || metadata.file_type().is_symlink() {
                return Err(AppError::Path(
                    "Engine save input is not a nonempty regular video file".into(),
                ));
            }
            let canonical = path
                .canonicalize()
                .map_err(|error| io_error(&value, error))?;
            if canonical.parent() != Some(root.as_path()) || !seen.insert(canonical.clone()) {
                return Err(AppError::Path(
                    "Engine save input is duplicated or escapes its save folder".into(),
                ));
            }
            files.push(canonical);
        }
        if segmented && output.exists() {
            let canonical = output
                .canonicalize()
                .map_err(|error| io_error(&clip.file_path, error))?;
            if !files.contains(&canonical) {
                return Err(AppError::Path(
                    "Segment destination already exists; original files preserved".into(),
                ));
            }
        }
        Ok(Self {
            root,
            files,
            segmented,
        })
    }
}

fn validate_location(path: &Path, root: &Path) -> AppResult<()> {
    if !path.is_absolute()
        || path
            .extension()
            .map_or(true, |ext| !ext.eq_ignore_ascii_case("mp4"))
    {
        return Err(AppError::Path(
            "Engine save path must be an absolute MP4 path".into(),
        ));
    }
    let parent = path.parent().and_then(|parent| parent.canonicalize().ok());
    if parent.as_deref() != Some(root) {
        return Err(AppError::Path(
            "Engine save path is outside the selected save folder".into(),
        ));
    }
    Ok(())
}

fn io_error(path: &str, source: std::io::Error) -> AppError {
    AppError::Io {
        action: "validate engine save input",
        path: path.into(),
        source,
    }
}
