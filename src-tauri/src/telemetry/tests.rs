use super::*;
use crate::contracts::{ClipRecord, SaveClipResult};

fn enrolled(root: &std::path::Path) {
    let key = enrollment::new_key().unwrap();
    Enrollment {
        server: "http://127.0.0.1:9".into(),
        device_id: "d-test".into(),
        friend_name: "sam".into(),
        public_key: key.public_key(),
        protected_key: enrollment::protect_key(&key).unwrap(),
        enrolled_at: now_text(),
        state: EnrollmentState::Active,
    }
    .save(&root.join("telemetry"))
    .unwrap();
}

fn records(data: &std::path::Path) -> Vec<clipture_telemetry_protocol::Record> {
    let mut lines = Vec::new();
    let mut files: Vec<_> = std::fs::read_dir(data.join("telemetry").join("journal")).unwrap().flatten().map(|e| e.path()).collect();
    files.sort();
    for file in files {
        for line in std::fs::read_to_string(file).unwrap().lines() {
            lines.push(serde_json::from_str(line).unwrap());
        }
    }
    lines
}

fn codes(data: &std::path::Path) -> Vec<String> {
    records(data)
        .into_iter()
        .map(|record| match record.body {
            Body::Event(event) => event.code,
            Body::RunStart(_) => "run_start".into(),
            Body::RunEnd { .. } => "run_end".into(),
            other => format!("{other:?}"),
        })
        .collect()
}

fn run_start() -> RunStart {
    RunStart { version: "1.9.0".into(), ..Default::default() }
}

#[test]
fn nothing_is_recorded_without_an_invite() {
    let data = tempfile::tempdir().unwrap();
    let service = TelemetryService::new(data.path(), false);
    service.begin_run(run_start());
    service.saved(&SaveClipResult { ok: true, ..Default::default() }, 10, 30);
    service.finish("quit");
    assert!(!data.path().join("telemetry").exists());
    assert_eq!(service.status().state, "off");
}

#[test]
fn a_run_records_saves_and_closes_cleanly() {
    let data = tempfile::tempdir().unwrap();
    enrolled(data.path());
    let service = TelemetryService::new(data.path(), false);
    service.begin_run(run_start());
    let game = ClipRecord { is_game: Some(true), game_or_app: "Deadlock".into(), fps: 60, ..Default::default() };
    let app = ClipRecord { is_game: Some(false), game_or_app: "Discord".into(), ..Default::default() };
    service.saved(&SaveClipResult { ok: true, clip: Some(game), ..Default::default() }, 900, 30);
    service.saved(&SaveClipResult { ok: true, clip: Some(app), ..Default::default() }, 900, 30);
    service.saved(&SaveClipResult { ok: false, message: r"Disk full writing D:\Clips\x.mp4".into(), ..Default::default() }, 50, 30);
    service.finish("quit");
    assert_eq!(codes(data.path()), ["run_start", "save", "save", "save_failed", "run_end"]);
    let all = records(data.path());
    let Body::Event(game_save) = &all[1].body else { panic!() };
    assert_eq!(game_save.data.get("game"), Some(&serde_json::json!("Deadlock")));
    let Body::Event(app_save) = &all[2].body else { panic!() };
    assert!(app_save.data.get("game").is_none(), "non-game app names are not sent");
    let Body::Event(failed) = &all[3].body else { panic!() };
    assert_eq!(failed.message.as_deref(), Some("Disk full writing <path>"));
    // The next run knows the last one ended cleanly.
    let service = TelemetryService::new(data.path(), false);
    service.begin_run(run_start());
    let all = records(data.path());
    let Body::RunStart(second) = &all.last().unwrap().body else { panic!() };
    assert_eq!(second.previous_clean, Some(true));
}

#[test]
fn an_unclean_exit_and_new_crashes_are_reported_on_the_next_start() {
    let data = tempfile::tempdir().unwrap();
    enrolled(data.path());
    std::fs::write(data.path().join("crash.log"), "[1] old crash before enrolling\n").unwrap();
    {
        let service = TelemetryService::new(data.path(), false);
        // Enrollment written by hand above: mimic enroll()'s crash offset.
        service.lock().journal.as_mut().unwrap().state.crash_log_offset = 31;
        service.begin_run(run_start());
        // No finish(): the process died.
    }
    let service = TelemetryService::new(data.path(), false);
    service.begin_run(run_start());
    assert_eq!(codes(data.path()), ["run_start", "run_start", "unclean_exit"]);

    let mut log = std::fs::read_to_string(data.path().join("crash.log")).unwrap();
    log.push_str(r"[2] Clipture 1.9.0 (controller) thread 'main' panicked at src\x.rs:1: boom in C:\Users\me\a" );
    log.push('\n');
    std::fs::write(data.path().join("crash.log"), log).unwrap();
    let service = TelemetryService::new(data.path(), false);
    service.begin_run(run_start());
    let all = records(data.path());
    let Body::Event(crash) = &all.last().unwrap().body else { panic!("{all:?}") };
    assert_eq!(crash.code, "crash");
    assert!(crash.message.as_deref().unwrap().ends_with("boom in <path>"), "{:?}", crash.message);
    assert!(!codes(data.path()).iter().skip(3).any(|code| code == "unclean_exit"), "a crash entry explains the exit");
}

#[test]
fn repeated_host_logs_become_one_event_with_a_count() {
    let data = tempfile::tempdir().unwrap();
    enrolled(data.path());
    let service = TelemetryService::new(data.path(), false);
    service.begin_run(run_start());
    for _ in 0..3 {
        service.host_log(Level::Warn, "clipture::media", "could not read thumbnail");
    }
    service.host_log(Level::Error, "clipture::save", "save folder missing");
    service.finish("quit");
    let events: Vec<_> = records(data.path())
        .into_iter()
        .filter_map(|record| match record.body {
            Body::Event(event) if event.code.starts_with("host_") => Some((event.code, event.count)),
            _ => None,
        })
        .collect();
    assert_eq!(events, [("host_warning".to_string(), 1), ("host_error".to_string(), 1), ("host_warning".to_string(), 2)]);
}

#[test]
fn stop_deletes_the_key_and_the_journal() {
    let data = tempfile::tempdir().unwrap();
    enrolled(data.path());
    let service = TelemetryService::new(data.path(), false);
    service.begin_run(run_start());
    assert_eq!(service.status().state, "active");
    assert_eq!(service.stop().state, "off");
    assert!(!data.path().join("telemetry").exists());
    service.saved(&SaveClipResult { ok: true, ..Default::default() }, 10, 30);
    assert!(!data.path().join("telemetry").exists());
}
