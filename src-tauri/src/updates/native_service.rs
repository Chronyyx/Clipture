use super::{activation, clock::now_rfc3339, component_download, inplace, manifest::invalid, model::UpdateState, runtime_store, service::{UpdateError, UpdateService}};
use tauri::AppHandle;

impl UpdateService {
    pub(super) async fn check_native(&self, app: &AppHandle, checked_at: String) -> Result<UpdateState, UpdateError> {
        // Only the background scheduler is once-per-startup. Manual checks must
        // remain retryable after an offline check or a later release.
        *self.native_pending.lock().unwrap_or_else(|error| error.into_inner()) = None;
        let key = activation::public_key();
        let root = runtime_store::root()?;
        if let Some(manifest) = inplace::installation::managed().and_then(|paths| inplace::launch::staged_upgrade(&paths)) {
            return Ok(self.publish(app, UpdateState::ready(manifest.version, checked_at)));
        }
        if let Ok(pending) = runtime_store::load_candidate(&root, "pending.json", &key) {
            if pending.manifest.newer_than(env!("CARGO_PKG_VERSION")) {
                *self.native_pending.lock().unwrap_or_else(|error| error.into_inner()) = Some(pending.signed);
                return Ok(self.publish(app, UpdateState::ready(pending.manifest.version, checked_at)));
            }
        }
        self.publish(app, UpdateState::checking(&self.get(), checked_at.clone()));
        match component_download::check(&key, env!("CARGO_PKG_VERSION")).await {
            Ok(Some((signed, manifest))) => {
                runtime_store::ensure_not_failed(&root, &manifest, &key).map_err(|error| self.fail(app, error))?;
                *self.native_pending.lock().unwrap_or_else(|error| error.into_inner()) = Some(signed);
                Ok(self.publish(app, UpdateState::available(manifest.version, checked_at)))
            }
            Ok(None) => Ok(self.publish(app, UpdateState::current(Some(env!("CARGO_PKG_VERSION").into()), checked_at))),
            Err(error) => Err(self.fail(app, error)),
        }
    }

    pub(super) async fn download_native(&self, app: &AppHandle) -> Result<(), UpdateError> {
        let signed = self.native_pending.lock().unwrap_or_else(|error| error.into_inner()).clone().ok_or(UpdateError::NoPendingUpdate)?;
        let key = activation::public_key();
        let manifest = signed.verify(&key)?;
        runtime_store::ensure_not_failed(&runtime_store::root()?, &manifest, &key).map_err(|error| self.fail(app, error))?;
        let checked_at = self.get().checked_at.unwrap_or_else(now_rfc3339);
        let total = manifest.files.iter().map(|file| file.size).sum::<u64>();
        let mut received = 0_u64;
        let mut previous = 101;
        self.publish(app, UpdateState::downloading(manifest.version.clone(), "Preparing signed runtime...".into(), checked_at.clone()));
        let progress = |size| {
            received += size;
            let percent = received.saturating_mul(100) / total;
            if percent != previous {
                previous = percent;
                self.publish(app, UpdateState::downloading(manifest.version.clone(), format!("Preparing update {percent}%"), checked_at.clone()));
            }
        };
        let staged = match inplace::installation::managed() {
            Some(paths) => inplace::stage::stage(&paths, &signed, &key, self.gate.as_ref(), None, progress).await.map(drop),
            None => runtime_store::stage(&runtime_store::root()?, &signed, &key, self.gate.as_ref(), progress).await,
        };
        staged.map_err(|error| self.fail(app, error))?;
        self.publish(app, UpdateState::ready(manifest.version, checked_at));
        Ok(())
    }

    pub(super) async fn install_native(&self, app: &AppHandle) -> Result<(), UpdateError> {
        if let Some(paths) = inplace::installation::managed() {
            return self.install_in_place(app, &paths);
        }
        let key = activation::public_key();
        let root = runtime_store::root()?;
        let candidate = runtime_store::load_candidate(&root, "pending.json", &key).map_err(|error| self.fail(app, error))?;
        if !candidate.manifest.newer_than(env!("CARGO_PKG_VERSION")) {
            return Err(self.fail(app, invalid("Runtime activation must advance the installed version")));
        }
        if activation::ui_executable().ok().as_ref() == Some(&candidate.directory.join("clipture.exe")) {
            let checked_at = self.get().checked_at.unwrap_or_else(now_rfc3339);
            self.publish(app, UpdateState::ready(candidate.manifest.version, checked_at));
            let app = app.clone();
            let dispatch = app.clone();
            dispatch.run_on_main_thread(move || {
                crate::app::ui_process::close(&app);
                if let Err(error) = crate::app::ui_process::open(&app) {
                    tracing::warn!(%error, "Compatible UI refresh failed");
                }
            }).map_err(|error| invalid(error.to_string()))?;
            return Ok(());
        }
        let lease = self.gate.reserve_installation().map_err(|reason| self.fail(app, UpdateError::Blocked(reason)))?;
        let child = activation::prepare(&candidate).map_err(|error| self.fail(app, error))?;
        child.commit().map_err(|error| self.fail(app, error))?;
        std::mem::forget(lease);
        self.gate.before_exit();
        crate::app::request_exit(app);
        Ok(())
    }

    /// Moves a runtime-store process onto its per-user installation and frees
    /// runtime-store versions nothing needs. Returns whether the stable
    /// executable changed, so startup entries can follow it.
    pub(super) async fn settle_installation(&self) -> Result<bool, UpdateError> {
        // A fresh per-user install is consolidated at once, which replaces the
        // installer's bundle-stamped controller with the signed bytes.
        let migrated = inplace::migrate::install_per_user(self.gate.as_ref()).await?;
        let moved = inplace::launch::consolidate(self.gate.as_ref()).await? || migrated;
        inplace::launch::retire_runtime_store();
        Ok(moved)
    }

    fn install_in_place(&self, app: &AppHandle, paths: &inplace::transaction::Paths) -> Result<(), UpdateError> {
        let manifest = inplace::launch::staged_upgrade(paths)
            .ok_or_else(|| self.fail(app, invalid("No verified update is staged")))?;
        if activation::ui_executable().ok().as_ref() == Some(&paths.staged(inplace::installation::EXECUTABLE)) {
            let checked_at = self.get().checked_at.unwrap_or_else(now_rfc3339);
            self.publish(app, UpdateState::ready(manifest.version, checked_at));
            let app = app.clone();
            let dispatch = app.clone();
            dispatch.run_on_main_thread(move || {
                crate::app::ui_process::close(&app);
                if let Err(error) = crate::app::ui_process::open(&app) {
                    tracing::warn!(%error, "Compatible UI refresh failed");
                }
            }).map_err(|error| invalid(error.to_string()))?;
            return Ok(());
        }
        // The supervisor verifies the stage before the lease stops capture,
        // so the only downtime is the rename and the new version's startup.
        let child = inplace::supervisor::prepare(paths).map_err(|error| self.fail(app, error))?;
        let lease = self.gate.reserve_installation().map_err(|reason| self.fail(app, UpdateError::Blocked(reason)))?;
        child.commit().map_err(|error| self.fail(app, error))?;
        std::mem::forget(lease);
        self.gate.before_exit();
        crate::app::request_exit(app);
        Ok(())
    }
}
