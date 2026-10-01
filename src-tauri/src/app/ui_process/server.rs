use super::{
    session::Session,
    wire::{self, Message, PROTOCOL_VERSION},
};
use crate::state::AppState;
use std::{
    io::Read,
    sync::{atomic::Ordering, Arc},
    thread,
};
use tauri::{AppHandle, Emitter, Manager};

pub fn read(app: AppHandle, session: Arc<Session>, mut reader: impl Read) {
    let slots = super::admission::Admission::new();
    // Media reads have their own budget: sharing the command slots let a burst
    // of thumbnail and icon calls fail a video's first range read, which leaves
    // the player black until the clip is reopened.
    let media_admission = Arc::new(crate::media::MediaAdmission::new());
    let mut ids = super::request_ids::RequestIds::default();
    while let Ok(Some(frame)) = wire::read_frame(&mut reader) {
        match frame.message {
            Message::Ready { version }
                if version == PROTOCOL_VERSION && !session.ready.load(Ordering::Acquire) =>
            {
                session.ready.store(true, Ordering::Release);
            }
            Message::Closing {} => session.close(),
            #[cfg(debug_assertions)]
            Message::SmokeReport { report } if session.smoke => {
                let _ = app.emit("clipture-smoke-result", report);
            }
            Message::Invoke { id, command, args }
                if session.ready.load(Ordering::Acquire) && ids.accept(id) =>
            {
                let Some(permit) = slots.acquire(Some(&command)) else {
                    let _ = session.output.send(
                        Message::Reply {
                            id,
                            result: Err("UI host is busy; retry shortly".into()),
                        }
                        .into(),
                    );
                    continue;
                };
                let app = app.clone();
                let session = session.clone();
                tauri::async_runtime::spawn(async move {
                    let _permit = permit;
                    let result =
                        super::dispatch::invoke(app.clone(), session.owner.clone(), &command, args)
                            .await;
                    if !session.alive.load(Ordering::Acquire) {
                        // An async request may finish after EOF and have just
                        // created a session; it must not outlive its UI owner.
                        app.state::<AppState>().media.release_ui(&session.owner);
                        return;
                    }
                    let output = session.output.clone();
                    let _ = tauri::async_runtime::spawn_blocking(move || {
                        output.send(Message::Reply { id, result }.into())
                    })
                    .await;
                });
            }
            message @ Message::Media { id, .. }
                if session.ready.load(Ordering::Acquire) && ids.accept(id) =>
            {
                let media = app.state::<AppState>().media.clone();
                let admission = media_admission.clone();
                let session = session.clone();
                thread::spawn(move || {
                    let response =
                        super::media_dispatch::respond(media, &admission, &session.owner, message);
                    if session.alive.load(Ordering::Acquire) {
                        let _ = session.output.send(response);
                    }
                });
            }
            _ => break, // Invalid direction, missing handshake, or invalid ID.
        }
    }
    session.alive.store(false, Ordering::Release);
    // Not closed by the user or Clipture: the window process died or sent
    // something invalid. Leave a trace; this otherwise leaves none.
    if !session.closing.load(Ordering::Acquire) {
        let ending = match session.exit_code() {
            Some(code) => format!("window process ended unexpectedly (exit code {code:#x})"),
            None => "window process stopped answering or sent an invalid message".to_owned(),
        };
        tracing::warn!("{ending}");
        crate::diagnostics::record_incident(&app.state::<AppState>().paths.data_dir, &ending);
    }
    session.stop();
    let state = app.state::<AppState>();
    state.media.release_ui(&session.owner);
    state.process_icons.clear();
    state.processes.invalidate();
    session.remove_listeners(&app);
    super::disconnected(&app, &session);
}
