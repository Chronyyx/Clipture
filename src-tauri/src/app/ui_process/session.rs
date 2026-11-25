use super::wire::Message;
use std::{
    process::Child,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Condvar, Mutex,
    },
    thread,
    time::{Duration, Instant},
};
use tauri::{AppHandle, Listener};

/// How long a new window process has to finish its handshake.
const STARTUP_PATIENCE: Duration = Duration::from_secs(20);

pub struct Session {
    pub owner: String,
    pub output: super::output::Output,
    pub alive: AtomicBool,
    pub ready: AtomicBool,
    pub closing: AtomicBool,
    // Read only by the debug-build smoke run.
    #[cfg_attr(not(debug_assertions), allow(dead_code))]
    pub smoke: bool,
    child: Mutex<Option<Child>>,
    #[cfg(windows)]
    job: Mutex<Option<crate::platform::windows::ChildJob>>,
    listeners: Mutex<Vec<tauri::EventId>>,
    // Wakes the startup watchdog when the window becomes ready or dies.
    settled: (Mutex<()>, Condvar),
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
            settled: (Mutex::new(()), Condvar::new()),
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
    /// The window finished its handshake.
    pub fn mark_ready(&self) {
        self.ready.store(true, Ordering::Release);
        self.wake_watchdog();
    }

    /// The window process is gone or no longer trusted.
    pub fn mark_dead(&self) {
        self.alive.store(false, Ordering::Release);
        self.wake_watchdog();
    }

    // Takes the lock first, so a wake between the watchdog's check and its
    // wait is not lost.
    fn wake_watchdog(&self) {
        drop(self.settled.0.lock().unwrap_or_else(|error| error.into_inner()));
        self.settled.1.notify_all();
    }

    pub fn stop(&self) {
        self.closing.store(true, Ordering::Release);
        self.mark_dead();
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

    /// The window process's exit code once it has ended, waiting up to a
    /// second for it to finish dying.
    pub fn exit_code(&self) -> Option<u32> {
        let deadline = Instant::now() + Duration::from_secs(1);
        loop {
            let status = self
                .child
                .lock()
                .unwrap_or_else(|error| error.into_inner())
                .as_mut()
                .and_then(|child| child.try_wait().ok().flatten());
            if let Some(status) = status {
                return status.code().map(|code| code as u32);
            }
            if Instant::now() >= deadline {
                return None;
            }
            thread::sleep(Duration::from_millis(50));
        }
    }

    /// Stops a window that is neither ready nor dead after `STARTUP_PATIENCE`.
    /// Sleeps until one of those happens instead of checking.
    pub fn startup_watchdog(self: &Arc<Self>) {
        let session = self.clone();
        thread::spawn(move || {
            let (lock, settled) = &session.settled;
            let guard = lock.lock().unwrap_or_else(|error| error.into_inner());
            let (guard, waited) = settled
                .wait_timeout_while(guard, STARTUP_PATIENCE, |_| {
                    session.alive.load(Ordering::Acquire) && !session.ready.load(Ordering::Acquire)
                })
                .unwrap_or_else(|error| error.into_inner());
            drop(guard);
            if waited.timed_out() {
                session.stop();
            }
        });
    }
}
