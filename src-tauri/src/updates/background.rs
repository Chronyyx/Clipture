use std::time::Duration;

use tauri::{AppHandle, Manager};

use super::service::UpdateService;

const INITIAL_CHECK_DELAY: Duration = Duration::from_secs(4);
const CHECK_INTERVAL: Duration = Duration::from_secs(30 * 60);

/// Keeps update discovery resident in the native controller. No WebView is
/// required, and download/install remain explicit user actions.
pub fn start(app: AppHandle, enabled: bool) {
    if !enabled {
        return;
    }
    tauri::async_runtime::spawn(async move {
        tokio::time::sleep(INITIAL_CHECK_DELAY).await;
        loop {
            if let Err(error) = app.state::<UpdateService>().check(&app).await {
                tracing::warn!(%error, "automatic update check failed");
            }
            tokio::time::sleep(CHECK_INTERVAL).await;
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn automatic_check_schedule_matches_the_legacy_host() {
        assert_eq!(INITIAL_CHECK_DELAY, Duration::from_secs(4));
        assert_eq!(CHECK_INTERVAL, Duration::from_secs(30 * 60));
    }
}
