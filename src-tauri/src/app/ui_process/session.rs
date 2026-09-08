use super::wire::Message;
use std::{
    process::Child,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
    thread,
    time::{Duration, Instant},
};
use tauri::{AppHandle, Listener};

pub struct Session {
    pub owner: String,
    pub output: super::output::Output,
    pub alive: AtomicBool,
    pub ready: AtomicBool,
    pub closing: AtomicBool,
    pub smoke: bool,
    child: Mutex<Option<Child>>,
    #[cfg(windows)]
    job: Mutex<Option<crate::platform::windows::ChildJob>>,
    listeners: Mutex<Vec<tauri::EventId>>,
}

impl Session {
    pub fn new(
        owner: String,
        output: super::output::Output,
        mut child: Child,
        smoke: bool,
    ) -> Result<Self, String> {
        #[cfg(windows)]
        let job = crate::platform::windows::ChildJob::attach(&child).map_err(|error| {
            // The child has received no bootstrap and cannot yet launch a WebView.
            let _ = child.kill();
            let _ = child.wait();
            format!("Could not own UI process tree: {error}")
        })?;
        Ok(Self {
            owner,
            output,
            alive: AtomicBool::new(true),
            ready: AtomicBool::new(false),
            closing: AtomicBool::new(false),
            smoke,
            child: Mutex::new(Some(child)),
            #[cfg(windows)]
            job: Mutex::new(Some(job)),
            listeners: Mutex::new(vec![]),
        })
    }

    pub fn watch_events(self: &Arc<Self>, app: &AppHandle) {
        for name in super::output::EVENTS {
            let weak = Arc::downgrade(self);
            let id = app.listen(name, move |event| {
                if let Some(session) = weak
                    .upgrade()
                    .filter(|session| session.alive.load(Ordering::Acquire))
                {
                    if let Ok(payload) = serde_json::from_str(event.payload()) {
                        // Coalescing preserves the last hint without blocking capture.
                        let _ = session.output.signal(
                            Message::Event {
                                name: name.into(),
                                payload,
                            }
                            .into(),
                        );
                    }
                }
            });
            self.listeners
                .lock()
                .unwrap_or_else(|error| error.into_inner())
                .push(id);
        }
    }

    pub fn remove_listeners(&self, app: &AppHandle) {
        for id in self
            .listeners
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .drain(..)
        {
            app.unlisten(id);
        }
    }

    pub fn close(self: &Arc<Self>) {
        if self.closing.swap(true, Ordering::AcqRel) {
            return;
        }
        let _ = self.output.signal(Message::Close {}.into());
        let session = self.clone();
        thread::spawn(move || {
            thread::sleep(Duration::from_secs(2));
            session.stop();
        });
    }

    /// Only the actual owned Child handle is used; PID reuse cannot target a
    /// different application. No engine or unrelated process is touched.
    pub fn stop(&self) {
        self.closing.store(true, Ordering::Release);
        self.alive.store(false, Ordering::Release);
        if let Some(mut child) = self
            .child
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .take()
        {
            if child.try_wait().ok().flatten().is_none() {
                let _ = child.kill();
            }
            let _ = child.wait();
        }
        // Also reap browser descendants if the worker crashed or the runtime
        // delayed shutdown. This job never contains the recorder or controller.
        #[cfg(windows)]
        drop(
            self.job
                .lock()
                .unwrap_or_else(|error| error.into_inner())
                .take(),
        );
    }

    pub fn startup_watchdog(self: &Arc<Self>) {
        let session = self.clone();
        thread::spawn(move || {
            let deadline = Instant::now() + Duration::from_secs(20);
            while session.alive.load(Ordering::Acquire) && !session.ready.load(Ordering::Acquire) {
                if Instant::now() >= deadline {
                    session.stop();
                    break;
                }
                thread::sleep(Duration::from_millis(50));
            }
        });
    }
}
