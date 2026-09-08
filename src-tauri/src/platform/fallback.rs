use std::path::Path;

use crate::{error::AppResult, processes::IconSource};

#[derive(Default)]
pub struct NativeIconSource;

impl IconSource for NativeIconSource {
    fn executable_icon_data_url(&self, _: &Path, _: u32) -> AppResult<Option<String>> {
        Ok(None)
    }
}
