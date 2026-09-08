use std::{
    env,
    ffi::OsString,
    path::{Path, PathBuf},
};

use crate::error::{AppError, AppResult};

pub const DATA_DIR_OVERRIDE: &str = "CLIPTURE_DATA_DIR";
pub const ENGINE_PATH_OVERRIDE: &str = "CLIPTURE_ENGINE_PATH";
pub const FFMPEG_PATH_OVERRIDE: &str = "CLIPTURE_FFMPEG_PATH";
pub const TEST_MODE_ENV: &str = "CLIPTURE_TEST_MODE";

#[derive(Clone, Debug)]
pub struct AppPaths {
    pub data_dir: PathBuf,
    pub settings_file: PathBuf,
    pub clips_file: PathBuf,
    pub sounds_dir: PathBuf,
    pub default_save_dir: PathBuf,
    pub engine_override: Option<PathBuf>,
    pub ffmpeg_override: Option<PathBuf>,
    pub test_mode: bool,
    /// Explicit profile overrides must never alter the daily OS integration.
    pub isolated_profile: bool,
    working_dir: PathBuf,
}

#[derive(Default)]
struct PathInputs {
    data_override: Option<OsString>,
    engine_override: Option<OsString>,
    ffmpeg_override: Option<OsString>,
    roaming_dir: Option<PathBuf>,
    video_dir: Option<PathBuf>,
    current_dir: Option<PathBuf>,
    test_mode: Option<OsString>,
}

impl AppPaths {
    pub fn discover() -> AppResult<Self> {
        Self::from_inputs(PathInputs {
            data_override: env::var_os(DATA_DIR_OVERRIDE),
            engine_override: env::var_os(ENGINE_PATH_OVERRIDE),
            ffmpeg_override: env::var_os(FFMPEG_PATH_OVERRIDE),
            roaming_dir: dirs::config_dir(),
            video_dir: dirs::video_dir(),
            current_dir: env::current_dir().ok(),
            test_mode: env::var_os(TEST_MODE_ENV),
        })
    }

    fn from_inputs(inputs: PathInputs) -> AppResult<Self> {
        let current_dir = inputs
            .current_dir
            .ok_or_else(|| AppError::Path("the current working directory is unavailable".into()))?;
        let test_mode = inputs.test_mode.as_deref().is_some_and(is_truthy);
        let isolated_profile = inputs
            .data_override
            .as_ref()
            .is_some_and(|path| !path.is_empty());
        if test_mode
            && inputs
                .data_override
                .as_ref()
                .is_none_or(|path| path.is_empty())
        {
            return Err(AppError::Path(format!(
                "{TEST_MODE_ENV} requires an isolated {DATA_DIR_OVERRIDE}"
            )));
        }
        let data_dir = match inputs.data_override {
            Some(path) if !path.is_empty() => absolute_from(path, &current_dir),
            _ => inputs
                .roaming_dir
                .ok_or_else(|| {
                    AppError::Path("the Windows roaming AppData directory is unavailable".into())
                })?
                .join("Clipture")
                .join("data"),
        };
        let default_save_dir = if isolated_profile {
            data_dir.join("clips")
        } else {
            inputs
                .video_dir
                .unwrap_or_else(|| current_dir.join("Videos"))
                .join("Clipture")
        };

        Ok(Self {
            settings_file: data_dir.join("settings.json"),
            clips_file: data_dir.join("clips.json"),
            sounds_dir: data_dir.join("sounds"),
            data_dir,
            default_save_dir,
            engine_override: inputs
                .engine_override
                .filter(|path| !path.is_empty())
                .map(|path| absolute_from(path, &current_dir)),
            ffmpeg_override: inputs
                .ffmpeg_override
                .filter(|path| !path.is_empty())
                .map(|path| absolute_from(path, &current_dir)),
            test_mode,
            isolated_profile,
            working_dir: current_dir,
        })
    }

    pub fn ensure_data_dir(&self) -> AppResult<()> {
        std::fs::create_dir_all(&self.data_dir).map_err(|source| AppError::Io {
            action: "create Clipture data directory",
            path: self.data_dir.clone(),
            source,
        })
    }

    pub fn authorize_write_path(&self, path: &Path) -> AppResult<()> {
        if !self.test_mode {
            return Ok(());
        }
        if !path.is_absolute()
            || path
                .components()
                .any(|component| component == std::path::Component::ParentDir)
        {
            return Err(AppError::Path(format!(
                "test-mode write path is not an absolute child path: {}",
                path.display()
            )));
        }
        let safety_root = self.data_dir.parent().unwrap_or(&self.data_dir);
        if !path.starts_with(safety_root) {
            return Err(AppError::Path(format!(
                "test-mode write escaped the isolated root {}: {}",
                safety_root.display(),
                path.display()
            )));
        }
        Ok(())
    }

    pub fn development_engine(&self) -> Option<PathBuf> {
        if let Some(path) = &self.engine_override {
            return Some(path.clone());
        }
        let root = &self.working_dir;
        [
            root.join("build/engine/Release/clipture_engine.exe"),
            root.join("build/engine/Debug/clipture_engine.exe"),
        ]
        .into_iter()
        .find(|candidate| candidate.is_file())
    }

    #[cfg(test)]
    pub fn test_fixture(root: &Path) -> Self {
        let data_dir = root.join("data");
        Self {
            settings_file: data_dir.join("settings.json"),
            clips_file: data_dir.join("clips.json"),
            sounds_dir: data_dir.join("sounds"),
            default_save_dir: root.join("clips"),
            data_dir,
            engine_override: Some(root.join("fake-engine.exe")),
            ffmpeg_override: Some(root.join("fake-ffmpeg.exe")),
            test_mode: true,
            isolated_profile: true,
            working_dir: root.to_owned(),
        }
    }
}

fn absolute_from(value: OsString, current_dir: &Path) -> PathBuf {
    let path = PathBuf::from(value);
    if path.is_absolute() {
        path
    } else {
        current_dir.join(path)
    }
}

fn is_truthy(value: &std::ffi::OsStr) -> bool {
    matches!(
        value.to_string_lossy().trim().to_ascii_lowercase().as_str(),
        "1" | "true" | "yes" | "on"
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn inputs() -> PathInputs {
        PathInputs {
            roaming_dir: Some(PathBuf::from(r"C:\Users\test\AppData\Roaming")),
            video_dir: Some(PathBuf::from(r"C:\Users\test\Videos")),
            current_dir: Some(PathBuf::from(r"C:\work\Clipture")),
            ..PathInputs::default()
        }
    }

    #[test]
    fn production_path_matches_the_electron_legacy_location() {
        let paths = AppPaths::from_inputs(inputs()).unwrap();
        assert_eq!(
            paths.data_dir,
            PathBuf::from(r"C:\Users\test\AppData\Roaming\Clipture\data")
        );
        assert_eq!(paths.settings_file, paths.data_dir.join("settings.json"));
        assert!(!paths.isolated_profile);
        assert_eq!(paths.clips_file, paths.data_dir.join("clips.json"));
    }

    #[test]
    fn data_override_wins_and_relative_overrides_are_workspace_relative() {
        let mut values = inputs();
        values.data_override = Some(OsString::from(r"fixtures\profile"));
        values.engine_override = Some(OsString::from(r"fixtures\fake-engine.exe"));
        values.ffmpeg_override = Some(OsString::from(r"C:\tools\ffmpeg.exe"));
        let paths = AppPaths::from_inputs(values).unwrap();
        assert!(paths.isolated_profile);
        assert_eq!(paths.default_save_dir, paths.data_dir.join("clips"));

        assert_eq!(
            paths.data_dir,
            PathBuf::from(r"C:\work\Clipture\fixtures\profile")
        );
        assert_eq!(
            paths.engine_override,
            Some(PathBuf::from(r"C:\work\Clipture\fixtures\fake-engine.exe"))
        );
        assert_eq!(
            paths.ffmpeg_override,
            Some(PathBuf::from(r"C:\tools\ffmpeg.exe"))
        );
    }

    #[test]
    fn test_mode_is_explicit_and_does_not_change_the_fixture_path() {
        let mut values = inputs();
        values.data_override = Some(OsString::from(r"C:\fixtures\profile"));
        values.test_mode = Some(OsString::from("1"));
        let paths = AppPaths::from_inputs(values).unwrap();
        assert!(paths.test_mode);
        assert_eq!(paths.data_dir, PathBuf::from(r"C:\fixtures\profile"));
        assert_eq!(
            paths.default_save_dir,
            PathBuf::from(r"C:\fixtures\profile\clips")
        );
    }

    #[test]
    fn test_mode_requires_an_explicit_data_override() {
        let mut values = inputs();
        values.test_mode = Some(OsString::from("true"));
        let error = AppPaths::from_inputs(values).unwrap_err();
        assert!(error.to_string().contains(DATA_DIR_OVERRIDE));
    }
}
