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
use tokio::sync::Semaphore;

pub fn read(app: AppHandle, session: Arc<Session>, mut reader: impl Read) {
    let slots = Arc::new(Semaphore::new(6));
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
                let Ok(permit) = slots.clone().try_acquire_owned() else {
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
                        app.state::<AppState>().media.release_owner(&session.owner);
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
                let Ok(permit) = slots.clone().try_acquire_owned() else {
                    let _ = session.output.send(super::media_dispatch::failure(
                        id,
                        503,
                        "UI media host is busy",
                    ));
                    continue;
                };
                let media = app.state::<AppState>().media.clone();
                let session = session.clone();
                thread::spawn(move || {
                    let _permit = permit;
                    let response = super::media_dispatch::respond(media, &session.owner, message);
                    if session.alive.load(Ordering::Acquire) {
                        let _ = session.output.send(response);
                    }
                });
            }
            _ => break, // Invalid direction, missing handshake, or invalid ID.
        }
    }
    session.alive.store(false, Ordering::Release);
    session.stop();
    let state = app.state::<AppState>();
    state.media.release_owner(&session.owner);
    state.process_icons.clear();
    state.processes.invalidate();
    session.remove_listeners(&app);
    super::disconnected(&app, &session);
}
