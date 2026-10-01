use std::{future::Future, time::Duration};

use tauri::{AppHandle, Manager};

use super::{model::UpdateStatus, service::UpdateService};

const INITIAL_CHECK_DELAY: Duration = Duration::from_secs(4);

pub fn start(app: AppHandle, enabled: bool) {
    if !enabled {
        return;
    }
    tauri::async_runtime::spawn(async move {
        tokio::time::sleep(INITIAL_CHECK_DELAY).await;
        let service = app.state::<UpdateService>();
        if let Err(error) = check_and_download_once(
            async { service.check(&app).await.map(|state| state.status) },
            || service.download(&app),
        )
        .await
        {
            tracing::warn!(%error, "startup update failed");
        }
    });
}

async fn check_and_download_once<E, D: Future<Output = Result<(), E>>>(
    check: impl Future<Output = Result<UpdateStatus, E>>,
    download: impl FnOnce() -> D,
) -> Result<(), E> {
    if check.await? == UpdateStatus::Available {
        download().await?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    #[test]
    fn startup_checks_once_and_downloads_only_available_updates() {
        assert_eq!(INITIAL_CHECK_DELAY, Duration::from_secs(4));
        tauri::async_runtime::block_on(async {
            for status in [
                UpdateStatus::Idle,
                UpdateStatus::Checking,
                UpdateStatus::Available,
                UpdateStatus::Downloading,
                UpdateStatus::Ready,
                UpdateStatus::Error,
            ] {
                let checks = Cell::new(0);
                let downloads = Cell::new(0);
                let result = check_and_download_once(
                    async {
                        checks.set(checks.get() + 1);
                        Ok::<_, &str>(status)
                    },
                    || async {
                        downloads.set(downloads.get() + 1);
                        Ok(())
                    },
                )
                .await;
                assert_eq!(result, Ok(()));
                assert_eq!(checks.get(), 1);
                assert_eq!(downloads.get(), usize::from(status == UpdateStatus::Available));
            }
        });
    }

    #[test]
    fn startup_check_failure_does_not_download() {
        tauri::async_runtime::block_on(async {
            let result = check_and_download_once(async { Err("offline") }, || async {
                panic!("must not download after a failed check");
            })
            .await;
            assert_eq!(result, Err("offline"));
        });
    }

    #[test]
    fn startup_download_failure_is_reported_without_rechecking() {
        tauri::async_runtime::block_on(async {
            let result = check_and_download_once(
                async { Ok(UpdateStatus::Available) },
                || async { Err("transfer failed") },
            )
            .await;
            assert_eq!(result, Err("transfer failed"));
        });
    }
}
