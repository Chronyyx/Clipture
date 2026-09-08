use super::wire::{self, Frame, Message};
use std::{
    collections::BTreeMap,
    io::Write,
    sync::{
        mpsc::{self, SyncSender, TrySendError},
        Arc, Mutex,
    },
    thread,
};

enum Item {
    Frame(Frame),
    Wake,
}
#[derive(Default)]
struct Hints {
    events: BTreeMap<String, Frame>,
    control: Option<Frame>,
}
#[derive(Clone)]
pub struct Output {
    sender: SyncSender<Item>,
    hints: Arc<Mutex<Hints>>,
}

impl Output {
    /// Only bounded background producers may block here, never capture/window callbacks.
    pub fn send(&self, frame: Frame) -> Result<(), String> {
        self.sender
            .send(Item::Frame(frame))
            .map_err(|_| "UI pipe closed".into())
    }
    pub fn try_send(&self, frame: Frame) -> Result<(), String> {
        self.sender
            .try_send(Item::Frame(frame))
            .map_err(|_| "UI pipe busy or closed".into())
    }
    /// State events are hints, not a replay log. Keep the latest value for each
    /// known event even when the two-frame media/reply queue is full.
    pub fn signal(&self, frame: Frame) -> Result<(), String> {
        {
            let mut hints = self.hints.lock().unwrap_or_else(|error| error.into_inner());
            match &frame.message {
                Message::Event { name, .. } if EVENTS.contains(&name.as_str()) => {
                    hints.events.insert(name.clone(), frame);
                }
                Message::Close {} => hints.control = Some(frame),
                Message::Focus {} => {
                    if !matches!(
                        hints.control.as_ref().map(|frame| &frame.message),
                        Some(Message::Close {})
                    ) {
                        hints.control = Some(frame);
                    }
                }
                _ => return Err("Unsupported coalesced UI signal".into()),
            }
        }
        match self.sender.try_send(Item::Wake) {
            Ok(()) | Err(TrySendError::Full(_)) => Ok(()),
            Err(TrySendError::Disconnected(_)) => Err("UI pipe closed".into()),
        }
    }
}

pub const EVENTS: [&str; 8] = [
    "host://ready",
    "settings://changed",
    "engine://hotkey",
    "engine://diagnostics",
    "engine://status",
    "clip://save",
    "library://changed",
    "updates://state-changed",
];

/// No resident writer while the UI is absent. Two ordinary frames plus eight
/// coalesced state hints and one lifecycle signal bound queued work. Each batch
/// drains hints even if the ordinary queue remains saturated.
pub fn start(mut writer: impl Write + Send + 'static) -> Output {
    let (sender, receiver) = mpsc::sync_channel(2);
    let hints = Arc::new(Mutex::new(Hints::default()));
    let pending = hints.clone();
    thread::spawn(move || {
        let result = (|| -> std::io::Result<()> {
            while let Ok(item) = receiver.recv() {
                if let Item::Frame(frame) = item {
                    wire::write_frame(&mut writer, &frame)?;
                }
                let batch =
                    std::mem::take(&mut *pending.lock().unwrap_or_else(|error| error.into_inner()));
                if let Some(frame) = batch.control {
                    wire::write_frame(&mut writer, &frame)?;
                }
                for frame in batch.events.into_values() {
                    wire::write_frame(&mut writer, &frame)?;
                }
            }
            Ok(())
        })();
        if let Err(error) = result {
            tracing::debug!(%error, "UI pipe writer closed");
        }
    });
    Output { sender, hints }
}

#[cfg(test)]
#[path = "output_tests.rs"]
mod tests;
