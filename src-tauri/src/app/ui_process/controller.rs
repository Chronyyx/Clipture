use super::{
    session::Session,
    wire::{Message, PROTOCOL_VERSION},
};
use crate::{
    error::{AppError, AppResult},
    state::AppState,
};
use std::{
    process::{Command, Stdio},
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc, Mutex,
    },
    thread,
};
use tauri::{AppHandle, Manager};

#[derive(Default)]
pub struct UiController {
    next: AtomicU64,
    current: Mutex<Slot>,
}

#[derive(Default)]
struct Slot {
    session: Option<Arc<Session>>,
    reopen: bool,
}

impl UiController {
    #[cfg(debug_assertions)]
    pub fn generation_for_smoke(&self) -> u64 {
        self.next.load(Ordering::Acquire)
    }

    pub fn open(&self, app: &AppHandle) -> AppResult<()> {
        let mut current = self
            .current
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if app.state::<AppState>().is_exiting() {
            return Err(AppError::Integration("Clipture is shutting down".into()));
        }
        if let Some(session) = current.session.as_ref() {
            // Wait for reader cleanup before reusing the WebView profile. A
            // stopped child can still own subscriptions/media until EOF cleanup.
            if session.closing.load(Ordering::Acquire) || !session.alive.load(Ordering::Acquire) {
                current.reopen = true;
                return Ok(());
            }
            session
                .output
                .signal(Message::Focus {}.into())
                .map_err(|_| {
                    AppError::Integration("The UI is busy; retry opening it shortly".into())
                })?;
            return Ok(());
        }
        let state = app.state::<AppState>();
        if state.is_exiting() {
            return Err(AppError::Integration("Clipture is shutting down".into()));
        }
        let webview_directory = state.paths.data_dir.join("webview2");
        std::fs::create_dir_all(&webview_directory).map_err(|source| AppError::Io {
            action: "create isolated WebView data directory",
            path: webview_directory.clone(),
            source,
        })?;
        let executable =
            std::env::current_exe().map_err(|error| AppError::Integration(error.to_string()))?;
        let mut command = Command::new(&executable);
        command
            .arg("--ui-worker")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x0800_0000);
        }
        let mut child = command.spawn().map_err(|source| AppError::Io {
            action: "start disposable UI",
            path: executable,
            source,
        })?;
        let stdin = child.stdin.take().expect("piped UI stdin");
        let stdout = child.stdout.take().expect("piped UI stdout");
        let output = super::output::start(stdin);
        let smoke = cfg!(debug_assertions)
            && state.paths.test_mode
            && std::env::args().any(|arg| arg == "--smoke-test");
        let session = Arc::new(
            Session::new(
                format!("ui-{}", self.next.fetch_add(1, Ordering::Relaxed)),
                output,
                child,
                smoke,
            )
            .map_err(AppError::Integration)?,
        );
        let bootstrap = Message::Bootstrap {
            version: PROTOCOL_VERSION,
            webview_directory,
            sounds_directory: state.paths.sounds_dir.clone(),
            smoke,
        };
        if session.output.send(bootstrap.into()).is_err() {
            session.stop();
            return Err(AppError::Integration("UI bootstrap pipe closed".into()));
        }
        session.watch_events(app);
        current.session = Some(session.clone());
        current.reopen = false;
        session.startup_watchdog();
        let app = app.clone();
        thread::spawn(move || super::server::read(app, session, stdout));
        Ok(())
    }

    pub fn close(&self) {
        if let Some(session) = self
            .current
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .session
            .as_ref()
        {
            session.close();
        }
    }

    pub fn is_open(&self) -> bool {
        self.current
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .session
            .as_ref()
            .is_some_and(|session| {
                session.alive.load(Ordering::Acquire) && session.ready.load(Ordering::Acquire)
            })
    }

    pub fn shutdown(&self) {
        let session = {
            let mut current = self
                .current
                .lock()
                .unwrap_or_else(|error| error.into_inner());
            current.reopen = false;
            current.session.take()
        };
        if let Some(session) = session {
            session.stop();
        }
    }

    pub fn disconnected(&self, session: &Arc<Session>) -> bool {
        let mut current = self
            .current
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if current
            .session
            .as_ref()
            .is_some_and(|current| Arc::ptr_eq(current, session))
        {
            current.session = None;
            return std::mem::take(&mut current.reopen);
        }
        false
    }
}
