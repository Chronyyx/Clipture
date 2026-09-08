use std::{
    fs,
    io::{BufWriter, Write},
    sync::RwLock,
};

use tempfile::NamedTempFile;

use crate::{
    contracts::ClipSettings,
    error::{AppError, AppResult},
    paths::AppPaths,
};

pub struct SettingsStore {
    paths: AppPaths,
    current: RwLock<ClipSettings>,
}

impl SettingsStore {
    pub fn load(paths: AppPaths) -> AppResult<Self> {
        paths.ensure_data_dir()?;
        let default_save = paths.default_save_dir.to_string_lossy().into_owned();
        let settings = if paths.settings_file.is_file() {
            let bytes = fs::read(&paths.settings_file).map_err(|source| AppError::Io {
                action: "read settings",
                path: paths.settings_file.clone(),
                source,
            })?;
            serde_json::from_slice::<ClipSettings>(&bytes)?.normalize(&default_save)
        } else {
            ClipSettings::defaults_with_save_folder(default_save)
        };
        Ok(Self {
            paths,
            current: RwLock::new(settings),
        })
    }

    pub fn get(&self) -> ClipSettings {
        self.current
            .read()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone()
    }

    pub fn save(&self, settings: ClipSettings) -> AppResult<ClipSettings> {
        let default_save = self.paths.default_save_dir.to_string_lossy();
        let normalized = settings.normalize(&default_save);
        let save_folder = std::path::Path::new(&normalized.save_folder);
        self.paths.authorize_write_path(save_folder)?;
        fs::create_dir_all(save_folder).map_err(|source| AppError::Io {
            action: "create clip save directory",
            path: save_folder.to_owned(),
            source,
        })?;
        self.persist_atomically(&normalized)?;
        *self
            .current
            .write()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = normalized.clone();
        Ok(normalized)
    }

    fn persist_atomically(&self, settings: &ClipSettings) -> AppResult<()> {
        let mut temporary =
            NamedTempFile::new_in(&self.paths.data_dir).map_err(|source| AppError::Io {
                action: "create temporary settings file",
                path: self.paths.data_dir.clone(),
                source,
            })?;
        {
            let mut writer = BufWriter::new(temporary.as_file_mut());
            serde_json::to_writer_pretty(&mut writer, settings)?;
            writer.write_all(b"\n").map_err(|source| AppError::Io {
                action: "write settings",
                path: self.paths.settings_file.clone(),
                source,
            })?;
            writer.flush().map_err(|source| AppError::Io {
                action: "flush settings",
                path: self.paths.settings_file.clone(),
                source,
            })?;
        }
        temporary
            .as_file()
            .sync_all()
            .map_err(|source| AppError::Io {
                action: "sync settings",
                path: self.paths.settings_file.clone(),
                source,
            })?;
        temporary
            .persist(&self.paths.settings_file)
            .map_err(|error| AppError::Io {
                action: "replace settings",
                path: self.paths.settings_file.clone(),
                source: error.error,
            })?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_settings_inside_the_injected_data_directory() {
        let root = tempfile::tempdir().unwrap();
        let paths = AppPaths::test_fixture(root.path());
        let store = SettingsStore::load(paths.clone()).unwrap();
        let mut settings = store.get();
        settings.clip_length_seconds = 77;
        store.save(settings).unwrap();

        let reopened = SettingsStore::load(paths).unwrap();
        assert_eq!(reopened.get().clip_length_seconds, 77);
    }
}
