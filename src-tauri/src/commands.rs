mod core;
mod diagnostics;
mod library;
mod media;
mod picker;
mod system;

pub use core::*;
pub use diagnostics::*;
pub use library::*;
pub use media::*;
pub use system::*;

use crate::error::AppResult;

pub type CommandResult<T> = Result<T, String>;

pub async fn blocking<T, F>(operation: F) -> CommandResult<T>
where
    T: Send + 'static,
    F: FnOnce() -> AppResult<T> + Send + 'static,
{
    tauri::async_runtime::spawn_blocking(operation)
        .await
        .map_err(|error| format!("background operation failed: {error}"))?
        .map_err(|error| error.to_string())
}
