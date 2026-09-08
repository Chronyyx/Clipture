use std::path::{Path, PathBuf};

use crate::{
    error::{AppError, AppResult},
    media::encode_base64,
    processes::IconSource,
};

use super::{icon_extract::extract_icon_png, installed_games::installed_executable};

/// Windows Shell adapter kept outside the process-domain service. Extracting
/// an icon is only performed while a UI asks for one; tray-idle keeps no GDI
/// objects or helper processes alive.
#[derive(Default)]
pub struct WindowsIconSource;

impl IconSource for WindowsIconSource {
    fn executable_icon_data_url(&self, executable: &Path, size: u32) -> AppResult<Option<String>> {
        let png =
            extract_icon_png(executable, size.clamp(16, 128)).map_err(AppError::Integration)?;
        Ok(png.map(|bytes| format!("data:image/png;base64,{}", encode_base64(&bytes))))
    }

    fn installed_executable(&self, candidate_names: &[String]) -> AppResult<Option<PathBuf>> {
        Ok(installed_executable(candidate_names))
    }
}
