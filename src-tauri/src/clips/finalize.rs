use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
};

use crate::{
    contracts::ClipRecord,
    error::{AppError, AppResult},
};

pub fn save_category(clip: &ClipRecord) -> String {
    let lower = clip.game_or_app.to_lowercase();
    if lower.contains("explorer") || lower.contains("desktop") {
        return "Explorer".into();
    }
    if clip.is_game != Some(true) {
        return "Apps".into();
    }
    super::safe_file_stem(&clip.game_or_app, "Other")
}

/// Publish without replacing an existing clip. The staged file and all engine
/// inputs remain owned by the caller until this succeeds. Hard links avoid an
/// extra clip-sized copy on NTFS; the temporary-copy fallback supports FAT/SMB.
pub fn publish_saved(source: &Path, destination: &Path) -> AppResult<()> {
    if fs::hard_link(source, destination).is_ok() {
        return Ok(());
    }
    let parent = destination
        .parent()
        .ok_or_else(|| AppError::Path("Clip destination has no parent".into()))?;
    let mut temporary = tempfile::NamedTempFile::new_in(parent)
        .map_err(|error| io_error("stage completed clip", destination, error))?;
    let mut input =
        fs::File::open(source).map_err(|error| io_error("open completed clip", source, error))?;
    std::io::copy(&mut input, temporary.as_file_mut())
        .map_err(|error| io_error("copy completed clip", destination, error))?;
    temporary
        .flush()
        .and_then(|_| temporary.as_file().sync_all())
        .map_err(|error| io_error("flush completed clip", destination, error))?;
    temporary
        .persist_noclobber(destination)
        .map_err(|error| io_error("publish completed clip", destination, error.error))?;
    Ok(())
}

pub fn destination(root: &Path, clip: &ClipRecord) -> AppResult<PathBuf> {
    let category = root.join(save_category(clip));
    if !category.exists() {
        fs::create_dir(&category)
            .map_err(|error| io_error("create clip category", &category, error))?;
    }
    let category = category
        .canonicalize()
        .map_err(|error| io_error("resolve clip category", &category, error))?;
    // A pre-existing junction must not redirect a save outside the selected root.
    if category.parent() != Some(root) || !category.is_dir() {
        return Err(AppError::Path(
            "Clip category escapes the selected save folder".into(),
        ));
    }
    let filename = Path::new(&clip.file_path)
        .file_name()
        .ok_or_else(|| AppError::Path("Saved clip has no filename".into()))?;
    let result = category.join(filename);
    if result
        .try_exists()
        .map_err(|error| io_error("inspect clip destination", &result, error))?
    {
        return Err(AppError::Path(
            "A clip already exists at the final destination; original files were preserved".into(),
        ));
    }
    Ok(result)
}

pub(crate) fn io_error(action: &'static str, path: &Path, source: std::io::Error) -> AppError {
    AppError::Io {
        action,
        path: path.into(),
        source,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn publishing_never_clobbers_an_existing_clip() {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("new.mp4");
        let destination = root.path().join("old.mp4");
        fs::write(&source, b"new").unwrap();
        fs::write(&destination, b"old").unwrap();
        assert!(publish_saved(&source, &destination).is_err());
        assert_eq!(fs::read(destination).unwrap(), b"old");
        assert_eq!(fs::read(source).unwrap(), b"new");
    }
}
