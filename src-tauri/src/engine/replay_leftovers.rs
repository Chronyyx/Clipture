//! The engine keeps its replay buffer in `<save folder>/.clipture-replay` as
//! `buffer-*.recording` files and deletes them when it shuts down cleanly. An
//! engine that crashes or is killed (updates, Task Manager, development
//! restarts) leaves its buffer behind, and these are never recoverable
//! (docs/replay-storage-progress.md), so they only fill the disk.
//!
//! A running engine holds its buffer open without delete sharing, so Windows
//! refuses to delete it: deleting every buffer that *can* be deleted removes
//! exactly the abandoned ones, even while another engine is recording.
use std::path::Path;

const FOLDER: &str = ".clipture-replay";

/// Deletes abandoned replay buffers; returns how many bytes were freed.
pub fn remove_abandoned_replay_buffers(save_folder: &Path) -> u64 {
    let Ok(entries) = std::fs::read_dir(save_folder.join(FOLDER)) else {
        return 0;
    };
    let mut freed = 0;
    for entry in entries.flatten() {
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if !(name.starts_with("buffer-") && name.ends_with(".recording")) {
            continue;
        }
        let Ok(metadata) = entry.metadata() else {
            continue;
        };
        if metadata.is_file() && std::fs::remove_file(entry.path()).is_ok() {
            freed += metadata.len();
        }
    }
    if freed > 0 {
        tracing::info!(freed_bytes = freed, "removed abandoned replay buffers");
    }
    freed
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::windows::fs::OpenOptionsExt;

    #[test]
    fn only_abandoned_buffers_are_removed() {
        let save = tempfile::tempdir().unwrap();
        let folder = save.path().join(FOLDER);
        std::fs::create_dir_all(&folder).unwrap();
        let abandoned = folder.join("buffer-1-2-0.recording");
        let live = folder.join("buffer-3-4-0.recording");
        let unrelated = folder.join("notes.txt");
        for path in [&abandoned, &live, &unrelated] {
            std::fs::write(path, b"bytes").unwrap();
        }
        // As the engine opens it: shared for reading only, never for delete.
        const FILE_SHARE_READ: u32 = 1;
        let _engine = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .share_mode(FILE_SHARE_READ)
            .open(&live)
            .unwrap();

        assert_eq!(remove_abandoned_replay_buffers(save.path()), 5);
        assert!(!abandoned.exists());
        assert!(live.exists(), "a running engine's buffer must survive");
        assert!(unrelated.exists());
        assert_eq!(remove_abandoned_replay_buffers(&save.path().join("missing")), 0);
    }
}
