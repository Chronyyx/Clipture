//! Lock-scoped owner of persisted sharing state. Every mutation is saved
//! before observers are told about it. No lock is held across an await.
use std::{
    sync::Mutex,
    time::{SystemTime, UNIX_EPOCH},
};

use crate::error::AppResult;

use super::store::{StateFile, StoredState};

/// Change hints for the UI. Implementations must not block.
pub trait SharingEvents: Send + Sync {
    fn changed(&self);
    fn library_changed(&self);
}

pub struct Core {
    file: StateFile,
    state: Mutex<StoredState>,
    pub events: Box<dyn SharingEvents>,
}

impl Core {
    pub fn load(file: StateFile, events: Box<dyn SharingEvents>) -> Self {
        let state = Mutex::new(file.load());
        Self {
            file,
            state,
            events,
        }
    }

    pub fn file(&self) -> &StateFile {
        &self.file
    }

    pub fn read<T>(&self, read: impl FnOnce(&StoredState) -> T) -> T {
        read(
            &self
                .state
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner()),
        )
    }

    /// Applies, persists and announces a change. A failed save rolls back.
    pub fn update<T>(&self, change: impl FnOnce(&mut StoredState) -> T) -> AppResult<T> {
        self.update_when(|state| (change(state), true))
    }

    /// Like `update`, but the closure reports whether anything changed, so
    /// no-op requests from peers never touch the disk.
    pub fn update_when<T>(
        &self,
        change: impl FnOnce(&mut StoredState) -> (T, bool),
    ) -> AppResult<T> {
        let result = {
            let mut guard = self
                .state
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            let mut next = guard.clone();
            let (result, changed) = change(&mut next);
            if !changed {
                return Ok(result);
            }
            self.file.save(&next)?;
            *guard = next;
            result
        };
        self.events.changed();
        Ok(result)
    }
}

pub fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_millis() as u64)
        .unwrap_or_default()
}
