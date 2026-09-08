use std::{collections::BTreeMap, time::SystemTime};

use tauri::{AppHandle, Manager, State};

use crate::{
    commands::{blocking, CommandResult},
    diagnostics::DiagnosticsInput,
    library::iso_utc,
    state::AppState,
};

#[tauri::command]
pub async fn export_diagnostics(
    app: AppHandle,
    state: State<'_, AppState>,
) -> CommandResult<Option<String>> {
    if state.paths.test_mode {
        return Ok(None);
    }
    let exported_at = iso_utc(SystemTime::now());
    let file_name = format!(
        "Clipture diagnostics {}.json",
        exported_at.replace([':', '.'], "-")
    );
    let mut picker = super::picker::desktop_picker(&app, "Export diagnostics")
        .set_file_name(file_name)
        .add_filter("JSON report", &["json"]);
    if let Ok(downloads) = app.path().download_dir() {
        picker = picker.set_directory(downloads);
    }
    let destination = super::picker::select(|done| picker.save_file(done))
        .await?
        .map(|path| path.into_path().map_err(|error| error.to_string()))
        .transpose()?;
    let Some(destination) = destination else {
        return Ok(None);
    };
    state
        .paths
        .authorize_write_path(&destination)
        .map_err(|error| error.to_string())?;

    let engine = state.engine.diagnostics().await.unwrap_or_else(|error| {
        crate::contracts::EngineDiagnostics::unavailable(error.to_string())
    });
    let hotkey = state.engine.hotkey_diagnostics();
    let builder = state.diagnostics.clone();
    let save_io_analysis = state.save_io.diagnostics_value();
    let frame_drop_analysis = state.engine.frame_drop_analysis();
    let save_timing_log = state.paths.data_dir.join("save-timing.log");
    let exported_for_report = exported_at.clone();
    let output_path = destination.clone();
    blocking(move || {
        let report = builder.build(DiagnosticsInput {
            exported_at: exported_for_report,
            runtime: BTreeMap::from([("tauri".into(), tauri::VERSION.into())]),
            hotkey,
            engine,
            frame_drop_analysis,
            save_io_analysis,
            save_timing_log: Some(save_timing_log),
        });
        builder.export(&report, &output_path)
    })
    .await?;
    Ok(Some(destination.to_string_lossy().into_owned()))
}
