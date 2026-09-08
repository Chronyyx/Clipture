mod frame_fields;
mod frame_freshness;
mod frame_recorder;
pub use frame_recorder::FrameDropRecorder;

use std::{
    collections::BTreeMap,
    fs::{self, File},
    io::{Read, Seek, SeekFrom, Write},
    path::{Path, PathBuf},
    sync::Arc,
};

use serde::Serialize;
use serde_json::Value;

use crate::{
    contracts::EngineDiagnostics,
    error::{AppError, AppResult},
};

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ApplicationInfo {
    pub name: String,
    pub version: String,
    pub packaged: bool,
}

#[derive(Clone, Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OperatingSystemInfo {
    pub platform: String,
    pub release: String,
    pub version: String,
    pub architecture: String,
    pub total_memory_bytes: u64,
    pub processor: String,
    pub logical_processors: usize,
}

pub trait SystemInfoProvider: Send + Sync {
    fn snapshot(&self) -> OperatingSystemInfo;
}

#[derive(Clone, Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct HotkeyDiagnostics {
    pub configured: String,
    pub normalized: String,
    pub ready: bool,
    pub armed: bool,
    pub status: String,
    pub trigger_count: u64,
    pub last_trigger_at: Option<String>,
    pub last_trigger_source: Option<String>,
}

pub struct DiagnosticsInput {
    pub exported_at: String,
    pub runtime: BTreeMap<String, String>,
    pub hotkey: HotkeyDiagnostics,
    pub engine: EngineDiagnostics,
    pub frame_drop_analysis: Value,
    pub save_io_analysis: Option<Value>,
    pub save_timing_log: Option<PathBuf>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveTimingDiagnostics {
    pub log_path: Option<String>,
    pub tail: String,
    pub tail_truncated: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DiagnosticsReport {
    pub report_version: u32,
    pub exported_at: String,
    pub application: ApplicationInfo,
    pub operating_system: OperatingSystemInfo,
    pub runtime: BTreeMap<String, String>,
    pub hotkey: HotkeyDiagnostics,
    pub diagnostics: EngineDiagnostics,
    pub frame_drop_analysis: Value,
    pub save_io_analysis: Option<Value>,
    pub save_timing: SaveTimingDiagnostics,
}

pub struct DiagnosticsBuilder {
    application: ApplicationInfo,
    system: Arc<dyn SystemInfoProvider>,
    maximum_log_tail_bytes: usize,
}

impl DiagnosticsBuilder {
    pub fn new(application: ApplicationInfo, system: Arc<dyn SystemInfoProvider>) -> Self {
        Self {
            application,
            system,
            maximum_log_tail_bytes: 128 * 1024,
        }
    }

    pub fn build(&self, input: DiagnosticsInput) -> DiagnosticsReport {
        let (tail, tail_truncated) = input
            .save_timing_log
            .as_deref()
            .map(|path| read_text_tail(path, self.maximum_log_tail_bytes))
            .unwrap_or_else(|| (String::new(), false));
        DiagnosticsReport {
            // Version 6 identifies the Rust/Tauri runtime field shape.
            report_version: 6,
            exported_at: input.exported_at,
            application: self.application.clone(),
            operating_system: self.system.snapshot(),
            runtime: input.runtime,
            hotkey: input.hotkey,
            diagnostics: input.engine,
            frame_drop_analysis: input.frame_drop_analysis,
            save_io_analysis: input.save_io_analysis,
            save_timing: SaveTimingDiagnostics {
                log_path: input
                    .save_timing_log
                    .map(|path| path.to_string_lossy().into_owned()),
                tail,
                tail_truncated,
            },
        }
    }

    /// `destination` must originate from the native save picker. This method
    /// intentionally does not invent or broaden a writable directory.
    pub fn export(&self, report: &DiagnosticsReport, destination: &Path) -> AppResult<()> {
        if destination.extension().and_then(|value| value.to_str()) != Some("json") {
            return Err(AppError::Path(
                "diagnostics export must use a .json file".into(),
            ));
        }
        if let Some(parent) = destination.parent() {
            fs::create_dir_all(parent).map_err(|source| AppError::Io {
                action: "create diagnostics export directory",
                path: parent.to_owned(),
                source,
            })?;
        }
        let mut file = File::create(destination).map_err(|source| AppError::Io {
            action: "create diagnostics report",
            path: destination.to_owned(),
            source,
        })?;
        serde_json::to_writer_pretty(&mut file, report)?;
        file.write_all(b"\n").map_err(|source| AppError::Io {
            action: "write diagnostics report",
            path: destination.to_owned(),
            source,
        })?;
        file.sync_all().map_err(|source| AppError::Io {
            action: "sync diagnostics report",
            path: destination.to_owned(),
            source,
        })
    }
}

fn read_text_tail(path: &Path, maximum_bytes: usize) -> (String, bool) {
    let Ok(mut file) = File::open(path) else {
        return (String::new(), false);
    };
    let Ok(length) = file.metadata().map(|metadata| metadata.len()) else {
        return (String::new(), false);
    };
    let truncated = length > maximum_bytes as u64;
    let start = length.saturating_sub(maximum_bytes as u64);
    if file.seek(SeekFrom::Start(start)).is_err() {
        return (String::new(), false);
    }
    let mut bytes = Vec::with_capacity((length - start) as usize);
    if file.read_to_end(&mut bytes).is_err() {
        return (String::new(), false);
    }
    (String::from_utf8_lossy(&bytes).into_owned(), truncated)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct FixtureSystem;
    impl SystemInfoProvider for FixtureSystem {
        fn snapshot(&self) -> OperatingSystemInfo {
            OperatingSystemInfo {
                platform: "fixture".into(),
                ..OperatingSystemInfo::default()
            }
        }
    }

    #[test]
    fn builds_and_exports_report_only_to_injected_destination() {
        let root = tempfile::tempdir().unwrap();
        let log = root.path().join("save-timing.log");
        fs::write(&log, "timing\n").unwrap();
        let builder = DiagnosticsBuilder::new(
            ApplicationInfo {
                name: "Clipture".into(),
                version: "test".into(),
                packaged: false,
            },
            Arc::new(FixtureSystem),
        );
        let report = builder.build(DiagnosticsInput {
            exported_at: "2025-01-01T00:00:00.000Z".into(),
            runtime: BTreeMap::from([("tauri".into(), "2".into())]),
            hotkey: HotkeyDiagnostics::default(),
            engine: EngineDiagnostics::unavailable("fixture"),
            frame_drop_analysis: Value::Null,
            save_io_analysis: None,
            save_timing_log: Some(log),
        });
        let destination = root.path().join("diagnostics.json");
        builder.export(&report, &destination).unwrap();
        let value: Value = serde_json::from_slice(&fs::read(destination).unwrap()).unwrap();
        assert_eq!(value["reportVersion"], 6);
        assert_eq!(value["operatingSystem"]["platform"], "fixture");
    }
}
