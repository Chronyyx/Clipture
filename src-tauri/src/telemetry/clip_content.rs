//! A saved clip's content verdict in telemetry: the `save` event carries the
//! engine's measurements, and a `clip_suspect` warning names a clip that came
//! out black, frozen, or recorded from a monitor other than the game's.

use clipture_telemetry_protocol::{Event, Level};

use crate::contracts::ClipContentCheck;

/// The measurements on every `save` event.
pub(super) fn with_content(event: Event, check: &ClipContentCheck) -> Event {
    event
        .with("source", check.recorded_source.clone())
        .with("blank", check.blank)
        .with("frozen", check.frozen)
        .with("maxKeyframeBytes", check.max_keyframe_bytes)
        .with("meanFrameBytes", check.mean_frame_bytes)
        .with("distinctSourceFrames", check.distinct_source_frames)
        .with("primaryMonitor", check.recorded_monitor_primary)
}

/// A `clip_suspect` warning when the clip likely shows nothing useful.
/// "Game on another monitor" counts only when a game was in front: an app on
/// another screen is normal.
pub(super) fn suspect(check: &ClipContentCheck, is_game: bool, game: Option<&str>) -> Option<Event> {
    let mut reasons = Vec::new();
    if check.blank {
        reasons.push("black");
    }
    if check.frozen {
        reasons.push("frozen");
    }
    if is_game && !check.foreground_on_recorded_monitor {
        reasons.push("game_on_other_monitor");
    }
    if reasons.is_empty() {
        return None;
    }
    let mut event = Event::new(Level::Warn, "clip_suspect")
        .with("reasons", reasons.join(","))
        .with("source", check.recorded_source.clone())
        .with("sourceSeconds", (check.source_seconds * 10.0).round() / 10.0)
        .with("spanSeconds", (check.span_seconds * 10.0).round() / 10.0)
        .with("maxKeyframeBytes", check.max_keyframe_bytes)
        .with("distinctSourceFrames", check.distinct_source_frames)
        .with("primaryMonitor", check.recorded_monitor_primary);
    if let Some(game) = game {
        event = event.with("game", game.to_string());
    }
    Some(event)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn healthy() -> ClipContentCheck {
        ClipContentCheck {
            span_seconds: 90.0,
            frames: 10_800,
            keyframes: 45,
            distinct_source_frames: 10_700,
            max_keyframe_bytes: 150_000,
            mean_frame_bytes: 50_000,
            recorded_source: "monitor".into(),
            source_seconds: 600.0,
            recorded_monitor_primary: true,
            foreground_on_recorded_monitor: true,
            ..ClipContentCheck::default()
        }
    }

    #[test]
    fn a_normal_clip_raises_nothing() {
        assert!(suspect(&healthy(), true, Some("Deadlock")).is_none());
    }

    #[test]
    fn a_black_game_window_clip_is_named_with_its_source() {
        let check = ClipContentCheck {
            blank: true,
            max_keyframe_bytes: 1_033,
            recorded_source: "game-window".into(),
            source_seconds: 1_234.56,
            ..healthy()
        };
        let event = suspect(&check, true, Some("Deadlock")).unwrap();
        assert_eq!(event.code, "clip_suspect");
        assert_eq!(event.level, Level::Warn);
        assert_eq!(event.data["reasons"], "black");
        assert_eq!(event.data["source"], "game-window");
        assert_eq!(event.data["sourceSeconds"], 1_234.6);
        assert_eq!(event.data["game"], "Deadlock");
    }

    #[test]
    fn frozen_and_wrong_monitor_combine() {
        let check = ClipContentCheck {
            frozen: true,
            distinct_source_frames: 1,
            foreground_on_recorded_monitor: false,
            recorded_monitor_primary: false,
            ..healthy()
        };
        let event = suspect(&check, true, None).unwrap();
        assert_eq!(event.data["reasons"], "frozen,game_on_other_monitor");
        assert_eq!(event.data["primaryMonitor"], false);
        assert!(!event.data.contains_key("game"));
    }

    #[test]
    fn an_app_on_another_monitor_is_normal() {
        let check = ClipContentCheck { foreground_on_recorded_monitor: false, ..healthy() };
        assert!(suspect(&check, false, None).is_none());
    }
}
