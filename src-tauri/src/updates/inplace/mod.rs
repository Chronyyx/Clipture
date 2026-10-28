//! In-place updates of a user-writable installation (ADR 0020). The
//! executable path never changes, nothing is installed twice, and a durable
//! journal lets any launch finish or undo an interrupted swap.
mod disk;
pub mod installation;
pub mod journal;
pub mod launch;
pub mod migrate;
pub mod stage;
pub mod supervisor;
pub mod transaction;
#[cfg(test)]
mod tests;
#[cfg(test)]
mod process_tests;
#[cfg(test)]
mod patch_tests;

/// Held exclusively by the supervisor that owns an applied transaction.
pub const LOCK: &str = "handoff.lock";

pub use disk::RealDisk;
