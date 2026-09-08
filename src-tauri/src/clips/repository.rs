use std::{
    fs,
    io::{BufWriter, Write},
    path::{Path, PathBuf},
    sync::Mutex,
};

use tempfile::NamedTempFile;

use crate::{
    contracts::ClipRecord,
    error::{AppError, AppResult},
};

/// Owns the legacy `clips.json` file. The lock makes read/modify/write
/// operations atomic within this process; callers never receive a mutable
/// reference to repository state.
pub struct ClipRepository {
    clips_file: PathBuf,
    gate: Mutex<()>,
}

impl ClipRepository {
    pub fn new(clips_file: impl Into<PathBuf>) -> Self {
        Self {
            clips_file: clips_file.into(),
            gate: Mutex::new(()),
        }
    }

    pub fn path(&self) -> &Path {
        &self.clips_file
    }

    pub fn load(&self) -> AppResult<Vec<ClipRecord>> {
        let _guard = self
            .gate
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        self.load_unlocked()
    }

    pub fn replace(&self, records: &[ClipRecord]) -> AppResult<()> {
        let _guard = self
            .gate
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        self.persist_unlocked(records)
    }

    pub fn update<T>(
        &self,
        operation: impl FnOnce(&mut Vec<ClipRecord>) -> AppResult<T>,
    ) -> AppResult<T> {
        let _guard = self
            .gate
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let mut records = self.load_unlocked()?;
        let result = operation(&mut records)?;
        self.persist_unlocked(&records)?;
        Ok(result)
    }

    pub fn append(&self, record: ClipRecord) -> AppResult<()> {
        self.update(|records| {
            records.retain(|candidate| {
                candidate.library_source.as_deref() != Some("imported") && candidate.id != record.id
            });
            records.insert(0, record);
            Ok(())
        })
    }

    fn load_unlocked(&self) -> AppResult<Vec<ClipRecord>> {
        if !self.clips_file.is_file() {
            return Ok(Vec::new());
        }
        let bytes = fs::read(&self.clips_file).map_err(|source| AppError::Io {
            action: "read clip records",
            path: self.clips_file.clone(),
            source,
        })?;
        let mut records: Vec<ClipRecord> = serde_json::from_slice(&bytes)?;
        // Imported records are an index of external folders and were never
        // persisted by Electron. Ignore them if an old/manual file contains one.
        records.retain(|record| record.library_source.as_deref() != Some("imported"));
        Ok(records)
    }

    fn persist_unlocked(&self, records: &[ClipRecord]) -> AppResult<()> {
        let parent = self
            .clips_file
            .parent()
            .ok_or_else(|| AppError::Path("clips.json has no parent directory".into()))?;
        fs::create_dir_all(parent).map_err(|source| AppError::Io {
            action: "create clip record directory",
            path: parent.to_owned(),
            source,
        })?;

        let persisted: Vec<_> = records
            .iter()
            .filter(|record| record.library_source.as_deref() != Some("imported"))
            .collect();
        let mut temporary = NamedTempFile::new_in(parent).map_err(|source| AppError::Io {
            action: "create temporary clip record file",
            path: parent.to_owned(),
            source,
        })?;
        {
            let mut writer = BufWriter::new(temporary.as_file_mut());
            serde_json::to_writer_pretty(&mut writer, &persisted)?;
            writer.write_all(b"\n").map_err(|source| AppError::Io {
                action: "write clip records",
                path: self.clips_file.clone(),
                source,
            })?;
            writer.flush().map_err(|source| AppError::Io {
                action: "flush clip records",
                path: self.clips_file.clone(),
                source,
            })?;
        }
        temporary
            .as_file()
            .sync_all()
            .map_err(|source| AppError::Io {
                action: "sync clip records",
                path: self.clips_file.clone(),
                source,
            })?;
        temporary
            .persist(&self.clips_file)
            .map_err(|error| AppError::Io {
                action: "replace clip records",
                path: self.clips_file.clone(),
                source: error.error,
            })?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn clip(id: &str, path: &Path) -> ClipRecord {
        ClipRecord {
            id: id.into(),
            title: id.into(),
            file_path: path.to_string_lossy().into_owned(),
            ..ClipRecord::default()
        }
    }

    #[test]
    fn append_is_deduplicated_and_round_trips_in_an_injected_directory() {
        let root = tempfile::tempdir().unwrap();
        let repository = ClipRepository::new(root.path().join("data/clips.json"));
        repository
            .append(clip("one", &root.path().join("one.mp4")))
            .unwrap();
        repository
            .append(clip("one", &root.path().join("new.mp4")))
            .unwrap();

        let records = repository.load().unwrap();
        assert_eq!(records.len(), 1);
        assert!(records[0].file_path.ends_with("new.mp4"));
    }
}
