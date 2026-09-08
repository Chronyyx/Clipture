use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
    time::Duration,
};

use serde::{Deserialize, Serialize};

use crate::error::AppResult;

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum NotificationPlacement {
    #[default]
    TopRight,
    TopLeft,
    BottomRight,
    BottomLeft,
    TopCenter,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NotificationKind {
    Saving,
    Saved,
    Failed,
}

#[derive(Clone, Debug)]
pub struct NotificationRequest {
    pub kind: NotificationKind,
    pub message: String,
    pub placement: NotificationPlacement,
    pub thumbnail: Option<PathBuf>,
    pub visible_for: Duration,
}

/// Implemented by a native Windows overlay/toast adapter. It deliberately has
/// no Tauri window or WebView parameter, so notifications remain available at
/// tray idle.
pub trait NotificationSink: Send + Sync {
    fn show(&self, request: &NotificationRequest) -> AppResult<()>;
    fn hide(&self) -> AppResult<()>;
}

#[derive(Default)]
pub struct SilentNotificationSink;

impl NotificationSink for SilentNotificationSink {
    fn show(&self, _: &NotificationRequest) -> AppResult<()> {
        Ok(())
    }

    fn hide(&self) -> AppResult<()> {
        Ok(())
    }
}

pub struct NotificationService {
    sink: Arc<dyn NotificationSink>,
    current: Mutex<Option<NotificationRequest>>,
}

impl NotificationService {
    pub fn new(sink: Arc<dyn NotificationSink>) -> Self {
        Self {
            sink,
            current: Mutex::new(None),
        }
    }

    pub fn saving(&self, placement: NotificationPlacement) -> AppResult<()> {
        self.show(NotificationRequest {
            kind: NotificationKind::Saving,
            message: "Saving clip...".into(),
            placement,
            thumbnail: None,
            visible_for: Duration::from_secs(30),
        })
    }

    pub fn saved(
        &self,
        placement: NotificationPlacement,
        thumbnail: Option<PathBuf>,
    ) -> AppResult<()> {
        self.show(NotificationRequest {
            kind: NotificationKind::Saved,
            message: "Clip saved!".into(),
            placement,
            thumbnail,
            visible_for: Duration::from_secs(4),
        })
    }

    pub fn failed(&self, placement: NotificationPlacement) -> AppResult<()> {
        self.show(NotificationRequest {
            kind: NotificationKind::Failed,
            message: "Clip failed".into(),
            placement,
            thumbnail: None,
            visible_for: Duration::from_secs(5),
        })
    }

    pub fn show(&self, request: NotificationRequest) -> AppResult<()> {
        self.sink.show(&request)?;
        *self
            .current
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(request);
        Ok(())
    }

    pub fn hide(&self) -> AppResult<()> {
        self.sink.hide()?;
        *self
            .current
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner()) = None;
        Ok(())
    }

    pub fn current(&self) -> Option<NotificationRequest> {
        self.current
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    #[derive(Default)]
    struct RecordingSink(AtomicUsize);
    impl NotificationSink for RecordingSink {
        fn show(&self, _: &NotificationRequest) -> AppResult<()> {
            self.0.fetch_add(1, Ordering::Relaxed);
            Ok(())
        }
        fn hide(&self) -> AppResult<()> {
            Ok(())
        }
    }

    #[test]
    fn notifications_do_not_require_a_renderer_or_window() {
        let sink = Arc::new(RecordingSink::default());
        let service = NotificationService::new(sink.clone());
        service
            .saved(NotificationPlacement::TopRight, None)
            .unwrap();
        assert_eq!(sink.0.load(Ordering::Relaxed), 1);
        assert_eq!(service.current().unwrap().kind, NotificationKind::Saved);
        service.hide().unwrap();
        assert!(service.current().is_none());
    }
}
