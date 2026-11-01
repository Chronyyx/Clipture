use crate::{
    contracts::{ClipRecord, ClipSettings},
    error::AppResult,
};

/// Runs off the async/UI thread. Success means a complete, published media file;
/// the coordinator remains responsible for committing its library record.
pub trait SavedClipProcessor: Send + Sync {
    fn process(&self, clip: ClipRecord, settings: &ClipSettings) -> AppResult<ClipRecord>;

    /// Background work once the clip is in the library (its thumbnail), so
    /// the library has it ready. Blocking; never delays the save result.
    fn after_saved(&self, _clip: &ClipRecord) {}
}
