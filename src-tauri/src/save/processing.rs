use crate::{
    contracts::{ClipRecord, ClipSettings},
    error::AppResult,
};

/// Runs off the async/UI thread. Success means a complete, published media file;
/// the coordinator remains responsible for committing its library record.
pub trait SavedClipProcessor: Send + Sync {
    fn process(&self, clip: ClipRecord, settings: &ClipSettings) -> AppResult<ClipRecord>;
}
