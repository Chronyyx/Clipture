//! Engine operations and configuration contract, independent of process I/O.
use super::*;

impl EngineClient {
    pub async fn diagnostics(self: &Arc<Self>) -> AppResult<EngineDiagnostics> {
        if self.paths.test_mode {
            return Ok(EngineDiagnostics::unavailable(
                "Engine diagnostics are disabled by CLIPTURE_TEST_MODE.",
            ));
        }
        let diagnostics: EngineDiagnostics = self
            .call("getDiagnostics", &(), DEFAULT_REQUEST_TIMEOUT)
            .await?;
        Ok(self.remember_diagnostics(diagnostics))
    }

    pub async fn list_audio_input_devices(self: &Arc<Self>) -> AppResult<Vec<AudioInputDevice>> {
        if self.paths.test_mode {
            return Ok(Vec::new());
        }
        self.call("listAudioInputDevices", &(), DEFAULT_REQUEST_TIMEOUT)
            .await
    }

    pub async fn list_display_devices(self: &Arc<Self>) -> AppResult<Vec<DisplayDevice>> {
        if self.paths.test_mode {
            return Ok(Vec::new());
        }
        self.call("listDisplayDevices", &(), DEFAULT_REQUEST_TIMEOUT)
            .await
    }

    pub async fn list_running_processes(
        self: &Arc<Self>,
        include_executable_paths: bool,
    ) -> AppResult<Vec<ActiveProcess>> {
        if self.paths.test_mode {
            return Ok(Vec::new());
        }
        self.call(
            "listRunningProcesses",
            &serde_json::json!({ "includeExecutablePaths": include_executable_paths }),
            DEFAULT_REQUEST_TIMEOUT,
        )
        .await
    }

    pub async fn process_executable_path(self: &Arc<Self>, process_id: u32) -> AppResult<String> {
        if self.paths.test_mode || process_id == 0 {
            return Ok(String::new());
        }
        self.call(
            "getProcessExecutablePath",
            &serde_json::json!({ "processId": process_id }),
            DEFAULT_REQUEST_TIMEOUT,
        )
        .await
    }

    pub async fn configure_settings(
        self: &Arc<Self>,
        settings: &crate::contracts::ClipSettings,
    ) -> AppResult<EngineDiagnostics> {
        if self.paths.test_mode {
            return Ok(EngineDiagnostics::unavailable(
                "Engine configuration is disabled by CLIPTURE_TEST_MODE.",
            ));
        }
        let displays = self.list_display_devices().await.unwrap_or_default();
        let configure = EngineConfigure::from_settings(settings, &displays);
        let generation = self.ensure_started()?;
        let unchanged = self
            .last_configure
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .as_ref()
            == Some(&configure)
            && self.configured_generation.load(Ordering::Acquire) == generation;
        if unchanged {
            return Ok(self.cached_diagnostics());
        }

        let diagnostics: EngineDiagnostics = self
            .call("configure", &configure, CONFIGURE_TIMEOUT)
            .await?;
        *self
            .last_configure
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(configure);
        self.configured_generation
            .store(generation, Ordering::Release);
        Ok(self.remember_diagnostics(diagnostics))
    }

    pub async fn configure_hotkey(self: &Arc<Self>, hotkey: &str) -> AppResult<HotkeyStatus> {
        self.hotkey_state.configuring(hotkey);
        *self
            .desired_hotkey
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = hotkey.to_owned();
        if self.paths.test_mode {
            return Ok(HotkeyStatus {
                status: "Native hotkey is disabled by CLIPTURE_TEST_MODE.".into(),
                ..HotkeyStatus::default()
            });
        }
        let status: HotkeyStatus = self
            .call(
                "configureHotkey",
                &serde_json::json!({ "hotkey": hotkey }),
                DEFAULT_REQUEST_TIMEOUT,
            )
            .await?;
        self.hotkey_state.configured(&status);
        Ok(status)
    }

    pub async fn save_clip(
        self: &Arc<Self>,
        duration_seconds: u32,
        save_folder: &str,
        analyze_io: bool,
    ) -> AppResult<SaveClipResult> {
        if self.paths.test_mode {
            return Ok(SaveClipResult::rejected(
                "Clip saving is disabled by CLIPTURE_TEST_MODE.",
            ));
        }
        self.call(
            "saveClip",
            &serde_json::json!({
                "durationSeconds": duration_seconds.clamp(5, 600),
                "saveFolder": save_folder,
                "analyzeIo": analyze_io,
            }),
            save_timeout(duration_seconds),
        )
        .await
    }
}
