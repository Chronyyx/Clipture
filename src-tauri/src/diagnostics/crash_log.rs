//! Release builds abort on panic, so a bug ends Clipture instantly and
//! leaves no trace. This hook appends what panicked, and where, to
//! `crash.log` in the data folder before the process ends, so a user can
//! send it.
use std::{
    io::Write,
    path::Path,
    time::{SystemTime, UNIX_EPOCH},
};

const FILE_NAME: &str = "crash.log";
/// The log is restarted once it passes this size.
const MAXIMUM_BYTES: u64 = 256 * 1024;

pub fn install_crash_log(data_dir: &Path) {
    let path = data_dir.join(FILE_NAME);
    let previous = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let thread = std::thread::current();
        let entry = entry(
            thread.name().unwrap_or("unnamed"),
            info.payload_as_str().unwrap_or("(non-text panic)"),
            &info
                .location()
                .map(|place| format!("{}:{}", place.file(), place.line()))
                .unwrap_or_default(),
        );
        let _ = append(&path, &entry);
        previous(info);
    }));
}

/// Something went wrong without a panic (for example the window process
/// died); recorded beside panics so one file tells the whole story.
pub fn record_incident(data_dir: &Path, what: &str) {
    let line = format!(
        "[{}] Clipture {} ({}) {what}\n",
        unix_seconds(),
        env!("CARGO_PKG_VERSION"),
        role()
    );
    let _ = append(&data_dir.join(FILE_NAME), &line);
}

fn entry(thread: &str, message: &str, location: &str) -> String {
    format!(
        "[{}] Clipture {} ({}) thread '{thread}' panicked at {location}: {message}\n",
        unix_seconds(),
        env!("CARGO_PKG_VERSION"),
        role(),
    )
}

fn unix_seconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs())
        .unwrap_or_default()
}

/// The controller and the disposable UI worker share one executable.
fn role() -> &'static str {
    if std::env::args().any(|argument| argument == "--ui-worker") {
        "ui worker"
    } else {
        "controller"
    }
}

fn append(path: &Path, entry: &str) -> std::io::Result<()> {
    let restart = std::fs::metadata(path).is_ok_and(|metadata| metadata.len() > MAXIMUM_BYTES);
    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .append(!restart)
        .write(true)
        .truncate(restart)
        .open(path)?;
    file.write_all(entry.as_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn entries_name_the_place_and_the_message() {
        let folder = tempfile::tempdir().unwrap();
        let path = folder.path().join(FILE_NAME);
        append(&path, &entry("main", "boom", "src/x.rs:7")).unwrap();
        append(&path, &entry("io", "again", "src/y.rs:9")).unwrap();
        let text = std::fs::read_to_string(&path).unwrap();
        assert!(text.contains("thread 'main' panicked at src/x.rs:7: boom"));
        assert_eq!(text.lines().count(), 2);
    }
}
