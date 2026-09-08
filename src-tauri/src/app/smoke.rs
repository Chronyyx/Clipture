//! Debug-only integration harness. Requires both an explicit CLI switch and
//! isolated test mode. Exercises the real WebView IPC and media protocols.
use crate::state::AppState;
use std::sync::{Arc, Mutex};
use tauri::{AppHandle, Listener, Manager, WebviewWindow};

fn enabled(app: &AppHandle) -> bool {
    app.state::<AppState>().paths.test_mode && std::env::args().any(|arg| arg == "--smoke-test")
}

fn cycles() -> usize {
    std::env::var("CLIPTURE_SMOKE_CYCLES")
        .ok()
        .and_then(|value| value.parse().ok())
        .unwrap_or(3)
        .clamp(1, 100)
}

fn window_only() -> bool {
    std::env::var("CLIPTURE_SMOKE_WINDOW_ONLY").as_deref() == Ok("1")
}

pub fn prepare_window<'a>(
    builder: tauri::WebviewWindowBuilder<'a, tauri::Wry, AppHandle>,
    app: &AppHandle,
) -> tauri::WebviewWindowBuilder<'a, tauri::Wry, AppHandle> {
    if !enabled(app) {
        return builder;
    }
    let builder = if std::env::var("CLIPTURE_SMOKE_NO_DRAG_DROP").as_deref() == Ok("1") {
        builder.disable_drag_drop_handler()
    } else {
        builder
    };
    if !window_only() {
        return builder;
    }
    builder.on_web_resource_request(|request, response| {
        if matches!(request.uri().path(), "/" | "/index.html") {
            *response.body_mut() = std::borrow::Cow::Borrowed(b"<!doctype html><title>Clipture lifecycle test</title><p>Isolated empty WebView</p>");
            response.headers_mut().remove("content-length");
        }
    })
}

pub(super) async fn resources(app: &AppHandle, cycle: usize, phase: &str) -> serde_json::Value {
    if std::env::var("CLIPTURE_SMOKE_RESOURCES").as_deref() != Ok("1") {
        return serde_json::Value::Null;
    }
    let directory = app.state::<AppState>().paths.data_dir.clone();
    let key = format!("{cycle}-{phase}");
    let request = serde_json::json!({"key": key, "phase": phase});
    let _ = std::fs::write(directory.join("resource-request.json"), request.to_string());
    for _ in 0..150 {
        tokio::time::sleep(std::time::Duration::from_millis(100)).await;
        if let Ok(bytes) = std::fs::read(directory.join(format!("resources-{key}.json"))) {
            let bytes = bytes.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(&bytes);
            if let Ok(value) = serde_json::from_slice(bytes) {
                return value;
            }
        }
    }
    serde_json::json!({"ok": false, "error": "resource monitor timed out"})
}

pub fn on_loaded(window: &WebviewWindow) {
    if enabled(window.app_handle()) {
        let script = if window_only() {
            r#"window.__TAURI_INTERNALS__.invoke('plugin:event|emit', {event: 'clipture-smoke-result', payload: {ok: true, checks: ['empty WebView lifecycle only']}})"#
        } else {
            include_str!("smoke.js")
        };
        let _ = window.eval(script);
    }
}

pub fn install(app: &AppHandle) {
    if !enabled(app) {
        return;
    }
    let reports = Arc::new(Mutex::new(Vec::<serde_json::Value>::new()));
    let handle = app.clone();
    app.listen("clipture-smoke-result", move |event| {
            let report = serde_json::from_str(event.payload()).unwrap_or_else(|_| serde_json::json!({"ok": false, "error": "invalid report"}));
        let handle = handle.clone();
        let reports = reports.clone();
        tauri::async_runtime::spawn(async move {
            let ok = report["ok"] == true;
            let count = {
                let mut reports = reports.lock().unwrap();
                reports.push(report);
                reports.len()
            };
            let open_resources = resources(&handle, count, "open").await;
            let close_handle = handle.clone();
            let _ = handle.run_on_main_thread(move || { let _ = super::windows::destroy_main(&close_handle); });
            tokio::time::sleep(std::time::Duration::from_secs(2)).await;
            #[cfg(windows)]
            if std::env::var("CLIPTURE_SMOKE_FREE_UNUSED_COM").as_deref() == Ok("1") {
                let _ = handle.run_on_main_thread(|| unsafe {
                    // Default unload delays preserve COM's worker-thread safety.
                    windows::Win32::System::Com::CoFreeUnusedLibraries();
                });
                tokio::time::sleep(std::time::Duration::from_millis(250)).await;
            }
            let disposable = handle.webview_windows().is_empty() && !super::ui_process::is_open(&handle);
            let closed_resources = resources(&handle, count, "closed").await;
            let resources_ok = [ &open_resources, &closed_resources ].iter()
                .all(|value| value.is_null() || value["ok"] == true);
            // Full process trees are already written by the monitor. Do not
            // retain every snapshot in the controller being memory-profiled.
            if !ok || !disposable || !resources_ok || count >= cycles() {
                let result = serde_json::json!({"ok": ok && disposable && resources_ok, "cycles": count,
                    "noWebviewsAfterClose": disposable, "reports": *reports.lock().unwrap()});
                finish(&handle, result);
            } else {
                let open_handle = handle.clone();
                let _ = handle.run_on_main_thread(move || { let _ = super::windows::open_main(&open_handle); });
            }
        });
    });
    let handle = app.clone();
    tauri::async_runtime::spawn(async move {
        tokio::time::sleep(std::time::Duration::from_secs(60 + cycles() as u64 * 40)).await;
        finish(
            &handle,
            serde_json::json!({"ok": false, "error": "WebView smoke test timed out"}),
        );
    });
}

fn finish(app: &AppHandle, mut result: serde_json::Value) {
    let path = app
        .state::<AppState>()
        .paths
        .data_dir
        .join("smoke-result.json");
    if let Some(reports) = result
        .get_mut("reports")
        .and_then(serde_json::Value::as_array_mut)
    {
        for (index, report) in reports.iter_mut().enumerate() {
            let read = |phase: &str| {
                let file = path
                    .parent()
                    .unwrap()
                    .join(format!("resources-{}-{phase}.json", index + 1));
                std::fs::read_to_string(file)
                    .ok()
                    .and_then(|text| {
                        serde_json::from_str::<serde_json::Value>(
                            text.trim_start_matches('\u{feff}'),
                        )
                        .ok()
                    })
                    .unwrap_or(serde_json::Value::Null)
            };
            report["resources"] = serde_json::json!({"open":read("open"),"closed":read("closed")});
        }
    }
    if let Err(error) = std::fs::write(path, serde_json::to_vec_pretty(&result).unwrap()) {
        tracing::error!(%error, "could not write smoke test result");
    }
    super::request_exit(app);
}
