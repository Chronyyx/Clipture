use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum UpdateStatus {
    Idle,
    Checking,
    Available,
    Downloading,
    Ready,
    Error,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateState {
    pub status: UpdateStatus,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub version: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub message: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub checked_at: Option<String>,
}

impl Default for UpdateState {
    fn default() -> Self {
        Self {
            status: UpdateStatus::Idle,
            version: None,
            message: None,
            checked_at: None,
        }
    }
}

impl UpdateState {
    pub(crate) fn checking(previous: &Self, checked_at: String) -> Self {
        Self {
            status: UpdateStatus::Checking,
            version: previous.version.clone(),
            message: Some("Checking for updates...".into()),
            checked_at: Some(checked_at),
        }
    }

    pub(crate) fn available(version: String, checked_at: String) -> Self {
        Self {
            status: UpdateStatus::Available,
            message: Some(format!("Update {version} is available.")),
            version: Some(version),
            checked_at: Some(checked_at),
        }
    }

    pub(crate) fn current(version: Option<String>, checked_at: String) -> Self {
        Self {
            status: UpdateStatus::Idle,
            version,
            message: Some("Clipture is up to date.".into()),
            checked_at: Some(checked_at),
        }
    }

    pub(crate) fn unavailable(message: String, checked_at: String) -> Self {
        Self {
            status: UpdateStatus::Idle,
            version: None,
            message: Some(message),
            checked_at: Some(checked_at),
        }
    }

    pub(crate) fn downloading(version: String, message: String, checked_at: String) -> Self {
        Self {
            status: UpdateStatus::Downloading,
            version: Some(version),
            message: Some(message),
            checked_at: Some(checked_at),
        }
    }

    pub(crate) fn ready(version: String, checked_at: String) -> Self {
        Self {
            status: UpdateStatus::Ready,
            message: Some(format!("Clipture {version} is ready to install.")),
            version: Some(version),
            checked_at: Some(checked_at),
        }
    }

    pub(crate) fn failed(previous: &Self, message: String) -> Self {
        Self {
            status: UpdateStatus::Error,
            version: previous.version.clone(),
            message: Some(message),
            checked_at: previous.checked_at.clone(),
        }
    }

    pub(crate) fn blocked(previous: &Self, message: String) -> Self {
        let status = match previous.status {
            UpdateStatus::Downloading => UpdateStatus::Available,
            status => status,
        };
        Self {
            status,
            version: previous.version.clone(),
            message: Some(message),
            checked_at: previous.checked_at.clone(),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum UpdateOperation {
    Download,
    Install,
}

impl UpdateOperation {
    pub(crate) fn label(self) -> &'static str {
        match self {
            Self::Download => "download",
            Self::Install => "installation",
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{UpdateState, UpdateStatus};

    #[test]
    fn update_state_uses_the_existing_renderer_shape() {
        let state = UpdateState::available("2.0.0".into(), "2026-09-04T01:02:03Z".into());
        let value = serde_json::to_value(state).unwrap();

        assert_eq!(value["status"], "available");
        assert_eq!(value["version"], "2.0.0");
        assert_eq!(value["checkedAt"], "2026-09-04T01:02:03Z");
        assert!(value.get("checked_at").is_none());
    }

    #[test]
    fn blocking_a_download_returns_to_an_actionable_state() {
        let downloading = UpdateState {
            status: UpdateStatus::Downloading,
            version: Some("2.0.0".into()),
            message: None,
            checked_at: None,
        };

        assert_eq!(
            UpdateState::blocked(&downloading, "Capture is busy.".into()).status,
            UpdateStatus::Available
        );
    }

    #[test]
    fn unavailable_checks_stay_idle() {
        let state = UpdateState::unavailable(
            "Updates are disabled by CLIPTURE_TEST_MODE.".into(),
            "2026-09-04T01:02:03Z".into(),
        );

        assert_eq!(state.status, UpdateStatus::Idle);
        assert!(state.message.unwrap().contains("CLIPTURE_TEST_MODE"));
    }
}
