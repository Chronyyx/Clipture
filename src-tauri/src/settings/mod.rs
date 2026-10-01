mod default_apps;

pub use default_apps::{seed_default_app_sources, DetectedAudioApp};

use std::{
    fs,
    io::{BufWriter, Write},
    sync::{Mutex, RwLock},
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
    /// The folder the native picker last returned, adoptable once as the save folder.
    granted_save_folder: Mutex<Option<String>>,
}

impl SettingsStore {
    pub fn load(paths: AppPaths) -> AppResult<Self> {
        paths.ensure_data_dir()?;
        let default_save = paths.default_save_dir.to_string_lossy().into_owned();
        let mut settings = if paths.settings_file.is_file() {
            let bytes = fs::read(&paths.settings_file).map_err(|source| AppError::Io {
                action: "read settings",
                path: paths.settings_file.clone(),
                source,
            })?;
            serde_json::from_slice::<ClipSettings>(&bytes)?.normalize(&default_save)
        } else {
            ClipSettings::defaults_with_save_folder(default_save)
        };
        // Persisted with the next save; until then detection simply repeats.
        if !paths.test_mode {
            seed_default_app_sources(&mut settings, &crate::platform::detect_default_audio_apps());
        }
        Ok(Self {
            paths,
            current: RwLock::new(settings),
            granted_save_folder: Mutex::new(None),
        })
    }

    /// The user picked `folder` in the native folder picker.
    pub fn grant_save_folder(&self, folder: &str) {
        *self.granted_save_folder.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(folder.to_owned());
    }

    /// Settings edited in the UI. Folders are host-owned: import roots and
    /// their titles change only through the import, rename and delete
    /// commands, and the save folder only to one the native picker returned.
    /// A compromised page therefore cannot widen what the library may delete.
    pub fn save_from_ui(&self, mut incoming: ClipSettings) -> AppResult<ClipSettings> {
        let current = self.get();
        incoming.imported_video_directories = current.imported_video_directories.clone();
        incoming.imported_video_titles = current.imported_video_titles.clone();
        if !same_folder(&incoming.save_folder, &current.save_folder) {
            let granted = self.granted_save_folder.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).take();
            if !granted.is_some_and(|folder| same_folder(&folder, &incoming.save_folder)) {
                tracing::warn!("ignored a save folder change that did not come from the folder picker");
                incoming.save_folder = current.save_folder;
            }
        }
        self.save(incoming)
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

fn same_folder(a: &str, b: &str) -> bool {
    let key = |value: &str| value.trim().replace('/', "\\").trim_end_matches('\\').to_lowercase();
    key(a) == key(b)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ui_saves_cannot_widen_library_folders() {
        let root = tempfile::tempdir().unwrap();
        let store = SettingsStore::load(AppPaths::test_fixture(root.path())).unwrap();
        let original = store.get();
        let mut forged = original.clone();
        forged.imported_video_directories = vec!["C:\\".into()];
        forged.imported_video_titles.insert("C:\\x.mp4".into(), "x".into());
        forged.save_folder = root.path().join("elsewhere").to_string_lossy().into_owned();
        forged.clip_length_seconds = 90;
        let saved = store.save_from_ui(forged).unwrap();
        assert_eq!(saved.imported_video_directories, original.imported_video_directories);
        assert_eq!(saved.imported_video_titles, original.imported_video_titles);
        assert_eq!(saved.save_folder, original.save_folder, "an unpicked folder is refused");
        assert_eq!(saved.clip_length_seconds, 90, "ordinary settings still save");
    }

    #[test]
    fn a_picked_save_folder_is_accepted_once() {
        let root = tempfile::tempdir().unwrap();
        let store = SettingsStore::load(AppPaths::test_fixture(root.path())).unwrap();
        let picked = root.path().join("picked").to_string_lossy().into_owned();
        store.grant_save_folder(&picked);
        let mut next = store.get();
        next.save_folder = picked.clone();
        assert_eq!(store.save_from_ui(next.clone()).unwrap().save_folder, picked);

        let other = root.path().join("other").to_string_lossy().into_owned();
        next.save_folder = other;
        assert_eq!(store.save_from_ui(next).unwrap().save_folder, picked, "the grant is single-use");
    }

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

    #[test]
    fn removed_fps_choices_normalize_on_load_and_save() {
        let root = tempfile::tempdir().unwrap();
        let paths = AppPaths::test_fixture(root.path());
        for fps in [144, 210, 240] {
            let store = SettingsStore::load(paths.clone()).unwrap();
            let mut settings = store.get();
            settings.fps = fps;
            std::fs::write(&paths.settings_file, serde_json::to_vec(&settings).unwrap()).unwrap();
            assert_eq!(SettingsStore::load(paths.clone()).unwrap().get().fps, 30);
            assert_eq!(store.save(settings).unwrap().fps, 30);
            assert_eq!(SettingsStore::load(paths.clone()).unwrap().get().fps, 30);
        }
    }

    #[test]
    fn supported_fps_survives_save_and_reload() {
        let root = tempfile::tempdir().unwrap();
        let paths = AppPaths::test_fixture(root.path());
        for fps in [24, 30, 60, 120] {
            let store = SettingsStore::load(paths.clone()).unwrap();
            let mut settings = store.get();
            settings.fps = fps;
            assert_eq!(store.save(settings).unwrap().fps, fps);
            assert_eq!(SettingsStore::load(paths.clone()).unwrap().get().fps, fps);
        }
    }
}
