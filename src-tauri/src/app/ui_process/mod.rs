//! Disposable UI-process host; see ADR 0004 and the migration checkpoint.
mod client;
mod controller;
mod dispatch;
mod media_dispatch;
mod output;
mod pending;
mod request_ids;
mod server;
mod session;
mod wire;
mod worker;
mod worker_media;

pub(crate) use controller::UiController;
use std::sync::Arc;
use tauri::{AppHandle, Manager};

pub fn enabled() -> bool {
    // The in-process path is retained only as an explicit migration oracle.
    std::env::var("CLIPTURE_UI_PROCESS").as_deref() != Ok("0")
}
pub fn open(app: &AppHandle) -> crate::error::AppResult<()> {
    app.state::<UiController>().open(app)
}
pub fn close(app: &AppHandle) {
    app.state::<UiController>().close();
}
pub fn is_open(app: &AppHandle) -> bool {
    app.try_state::<UiController>()
        .is_some_and(|controller| controller.is_open())
}
pub fn shutdown(app: &AppHandle) {
    if let Some(controller) = app.try_state::<UiController>() {
        controller.shutdown();
    }
}
fn disconnected(app: &AppHandle, session: &Arc<session::Session>) {
    if app.state::<UiController>().disconnected(session) {
        let reopening = app.clone();
        let _ = app.run_on_main_thread(move || {
            if let Err(error) = open(&reopening) {
                tracing::warn!(%error, "could not reopen disposable UI");
            }
        });
    }
}

pub fn run_if_worker() -> bool {
    if !std::env::args().any(|arg| arg == "--ui-worker") {
        return false;
    }
    if let Err(error) = worker::run() {
        eprintln!("Clipture UI worker failed: {error}");
        std::process::exit(1);
    }
    true
}

#[cfg(debug_assertions)]
pub fn crash_for_smoke(app: &AppHandle) -> bool {
    let isolated = app.state::<crate::state::AppState>().paths.isolated_profile;
    if !isolated || !std::env::args().any(|arg| arg == "--capture-smoke") {
        return false;
    }
    shutdown(app);
    true
}
