use std::{future::Future, time::Duration};

use tauri::{AppHandle, Manager};

use super::{model::UpdateStatus, service::UpdateService};

const INITIAL_CHECK_DELAY: Duration = Duration::from_secs(4);
/// Clipture usually starts with Windows and runs for days; checking only at
/// startup would leave it on an old version until the next reboot.
const RECHECK_INTERVAL: Duration = Duration::from_secs(6 * 60 * 60);
/// While a clip saves (or the recorder state is unreadable), look again soon.
const BUSY_RETRY: Duration = Duration::from_secs(5 * 60);

pub fn start(app: AppHandle, enabled: bool) {
    if !enabled {
        return;
    }
    tauri::async_runtime::spawn(async move {
        tokio::time::sleep(INITIAL_CHECK_DELAY).await;
        let service = app.state::<UpdateService>();
        match service.settle_installation().await {
            #[cfg(windows)]
            Ok(true) => {
                if let Err(error) = crate::app::autostart::retarget() {
                    tracing::warn!(%error, "Could not point the startup entry at the installation");
                }
            }
            Ok(_) => {}
            Err(error) => tracing::warn!(%error, "Could not consolidate the runtime into the installation"),
        }
        if let Err(error) = check_and_download_once(
            async { service.check(&app).await.map(|state| state.status) },
            || service.download(&app),
        )
        .await
        {
            tracing::warn!(%error, "startup update failed");
        }
        loop {
            tokio::time::sleep(RECHECK_INTERVAL).await;
            wait_until_idle(|| service.background_blocked(), BUSY_RETRY).await;
            // A staged update makes `check` return at once, so nothing is
            // fetched twice; a failure is retried at the next interval.
            if let Err(error) = check_and_download_once(
                async { service.check(&app).await.map(|state| state.status) },
                || service.download(&app),
            )
            .await
            {
                tracing::warn!(%error, "periodic update check failed");
            }
        }
    });
}

async fn wait_until_idle(blocked: impl Fn() -> bool, retry: Duration) {
    while blocked() {
        tokio::time::sleep(retry).await;
    }
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
    fn periodic_checks_run_every_few_hours_and_wait_out_saves() {
        assert_eq!(RECHECK_INTERVAL, Duration::from_secs(6 * 60 * 60));
        let polls = Cell::new(0);
        tauri::async_runtime::block_on(wait_until_idle(
            || {
                polls.set(polls.get() + 1);
                polls.get() < 3
            },
            Duration::from_millis(1),
        ));
        assert_eq!(polls.get(), 3, "blocked twice, then idle");
        tauri::async_runtime::block_on(wait_until_idle(|| false, Duration::from_secs(3600)));
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
