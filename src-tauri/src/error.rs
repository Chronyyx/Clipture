use std::{io, path::PathBuf};

pub type AppResult<T> = Result<T, AppError>;

#[derive(Debug, thiserror::Error)]
pub enum AppError {
    #[error("{0}")]
    Path(String),
    #[error("could not {action} at {path}: {source}")]
    Io {
        action: &'static str,
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("settings are not valid JSON: {0}")]
    SettingsJson(#[from] serde_json::Error),
    #[error("native engine error: {0}")]
    Engine(String),
    #[error("application integration error: {0}")]
    Integration(String),
}

impl From<tauri::Error> for AppError {
    fn from(value: tauri::Error) -> Self {
        Self::Integration(value.to_string())
    }
}
