//! A `tracing` layer that passes the host's own warnings and errors to
//! telemetry (`host_warning` / `host_error`). Only Clipture's targets, never
//! telemetry's own (a failing upload must not log itself into the journal).
use clipture_telemetry_protocol::Level;
use std::fmt::Write;
use tracing::{field::Field, Event, Subscriber};
use tracing_subscriber::{layer::Context, Layer};

pub struct TelemetryLayer;

pub fn layer() -> TelemetryLayer {
    TelemetryLayer
}

#[derive(Default)]
struct Message(String);

impl tracing::field::Visit for Message {
    fn record_debug(&mut self, field: &Field, value: &dyn std::fmt::Debug) {
        if field.name() == "message" {
            let _ = write!(self.0, "{value:?}");
            if !self.0.is_empty() && self.0.starts_with(' ') {
                self.0 = self.0.trim_start().to_string();
            }
        } else {
            let _ = write!(self.0, "{}{}={value:?}", if self.0.is_empty() { "" } else { " " }, field.name());
        }
    }

    fn record_str(&mut self, field: &Field, value: &str) {
        if field.name() == "message" {
            self.0.insert_str(0, value);
        } else {
            let _ = write!(self.0, "{}{}={value}", if self.0.is_empty() { "" } else { " " }, field.name());
        }
    }
}

/// The host's own modules only. `clipture_engine` is the engine's stderr,
/// relayed line by line: its debug chatter is not a warning.
fn wanted(target: &str) -> bool {
    (target == "clipture" || target.starts_with("clipture::")) && !target.starts_with("clipture::telemetry")
}

impl<S: Subscriber> Layer<S> for TelemetryLayer {
    fn on_event(&self, event: &Event<'_>, _context: Context<'_, S>) {
        let metadata = event.metadata();
        let level = match *metadata.level() {
            tracing::Level::ERROR => Level::Error,
            tracing::Level::WARN => Level::Warn,
            _ => return,
        };
        if !wanted(metadata.target()) {
            return;
        }
        let Some(service) = super::SINK.get().and_then(std::sync::Weak::upgrade) else { return };
        let mut message = Message::default();
        event.record(&mut message);
        service.host_log(level, metadata.target(), &message.0);
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn only_clipture_targets_outside_telemetry() {
        assert!(super::wanted("clipture::save::coordinator"));
        assert!(!super::wanted("clipture::telemetry::uploader"));
        assert!(!super::wanted("iroh::magicsock"));
        assert!(!super::wanted("clipture_engine"), "engine stderr is not a host warning");
    }
}
