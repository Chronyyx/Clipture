use std::{
    sync::{
        atomic::{AtomicBool, Ordering},
        Mutex,
    },
    time::SystemTime,
};

use serde::Serialize;
use serde_json::{json, Value};

use crate::library::iso_utc;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveIoAnalyzerState {
    pub available: bool,
    pub armed: bool,
    pub trace_ready: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub captured_at: Option<String>,
}

#[derive(Clone, Debug)]
struct SaveIoCapture {
    captured_at: String,
    clip_file_path: Option<String>,
    analyses: Vec<Value>,
}

/// Debug-only save I/O tracing state. The engine owns the measurements; this
/// service owns only the one-shot arm flag and the most recent bounded result.
pub struct SaveIoAnalyzer {
    available: bool,
    armed: AtomicBool,
    capture: Mutex<Option<SaveIoCapture>>,
}

impl SaveIoAnalyzer {
    pub fn new(available: bool) -> Self {
        Self {
            available,
            armed: AtomicBool::new(false),
            capture: Mutex::new(None),
        }
    }

    pub fn state(&self) -> SaveIoAnalyzerState {
        let capture = self
            .capture
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        SaveIoAnalyzerState {
            available: self.available,
            armed: self.available && self.armed.load(Ordering::Acquire),
            trace_ready: capture.is_some(),
            captured_at: capture.as_ref().map(|capture| capture.captured_at.clone()),
        }
    }

    pub fn set_armed(&self, armed: bool) -> SaveIoAnalyzerState {
        let armed = self.available && armed;
        self.armed.store(armed, Ordering::Release);
        if armed {
            *self
                .capture
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner()) = None;
        }
        self.state()
    }

    /// Consumes the flag once for the next accepted save request.
    pub fn begin_save(&self) -> bool {
        self.available && self.armed.swap(false, Ordering::AcqRel)
    }

    pub fn finish_save(&self, analyses: Option<Vec<Value>>, clip_file_path: Option<String>) {
        let Some(analyses) = analyses.filter(|analyses| !analyses.is_empty()) else {
            return;
        };
        *self
            .capture
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(SaveIoCapture {
            captured_at: iso_utc(SystemTime::now()),
            clip_file_path,
            analyses,
        });
    }

    pub fn diagnostics_value(&self) -> Option<Value> {
        self.capture
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .as_ref()
            .map(|capture| {
                json!({
                    "capturedAt": capture.captured_at,
                    "clipFilePath": capture.clip_file_path,
                    "analyses": capture.analyses,
                })
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn arm_is_one_shot_and_rearming_clears_the_previous_trace() {
        let analyzer = SaveIoAnalyzer::new(true);
        assert!(analyzer.set_armed(true).armed);
        assert!(analyzer.begin_save());
        assert!(!analyzer.begin_save());
        analyzer.finish_save(Some(vec![json!({ "bytes": 12 })]), Some("clip.mp4".into()));
        assert!(analyzer.state().trace_ready);
        assert!(!analyzer.set_armed(true).trace_ready);
    }

    #[test]
    fn unavailable_analyzer_can_never_be_armed() {
        let analyzer = SaveIoAnalyzer::new(false);
        assert!(!analyzer.set_armed(true).armed);
        assert!(!analyzer.begin_save());
    }
}
