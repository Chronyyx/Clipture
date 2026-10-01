use serde::Serialize;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum ClipRepairPhase {
    #[default]
    Idle,
    Checking,
    Checked,
    Repairing,
    Done,
}

/// Progress of the user-started "Fix clips" job. The job lives in the host,
/// so closing the window neither cancels it nor loses this state.
#[derive(Clone, Debug, Default, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ClipRepairStatus {
    pub phase: ClipRepairPhase,
    pub checked: u32,
    pub total: u32,
    pub needs_repair: u32,
    pub needs_repair_bytes: u64,
    pub repaired: u32,
    pub failed: u32,
    pub current_title: Option<String>,
    pub message: Option<String>,
}
