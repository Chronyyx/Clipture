use super::{
    pending::Pending,
    wire::{self, Frame, Message},
};
use serde_json::Value;
use std::{sync::Arc, time::Duration};
use tauri::{AppHandle, Emitter, Manager};

pub struct Client {
    pub output: super::output::Output,
    pub pending: Arc<Pending>,
}

impl Client {
    pub async fn request(
        &self,
        make: impl FnOnce(u64) -> Message,
        timeout: Duration,
    ) -> Result<Frame, String> {
        let ticket = self.pending.reserve()?;
        let frame = make(ticket.id).into();
        let output = self.output.clone();
        tauri::async_runtime::spawn_blocking(move || output.send(frame))
            .await
            .map_err(|error| error.to_string())?
            .map_err(|_| "Controller pipe closed".to_string())?;
        ticket.wait(timeout).await
    }

    pub async fn invoke(&self, command: String, args: Value) -> Result<Value, String> {
        let timeout = if command == "save_clip" {
            Duration::from_secs(1800)
        } else {
            Duration::from_secs(600)
        };
        match self
            .request(|id| Message::Invoke { id, command, args }, timeout)
            .await?
            .message
        {
            Message::Reply { result, .. } => result,
            _ => Err("Controller returned an unexpected command response".into()),
        }
    }
}

pub fn read(app: AppHandle, client: Arc<Client>) {
    let mut reader = std::io::stdin();
    while let Ok(Some(frame)) = wire::read_frame(&mut reader) {
        match frame.message {
            Message::Reply { .. } | Message::MediaReply { .. } => {
                client.pending.complete(frame);
            }
            Message::Event { name, payload } => {
                let _ = app.emit(&name, payload);
            }
            Message::Focus {} => {
                if let Some(window) = app.get_webview_window("main") {
                    let _ = window.unminimize();
                    let _ = window.show();
                    let _ = window.set_focus();
                }
            }
            Message::Close {} => {
                break;
            }
            _ => break,
        }
    }
    client.pending.close("Controller disconnected");
    // Loss of the private parent connection never promotes the UI to a recorder.
    super::worker::close_window(&app);
}
