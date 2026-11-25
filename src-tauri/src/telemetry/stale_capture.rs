//! Whether the recording is repeating an old picture while the game in front
//! keeps drawing new ones. Capture that receives no new frames is normal for
//! a still screen, so "few fresh frames" alone means nothing: the verdict
//! needs evidence the game is drawing that does not come from the capture.
//!
//! - Recording the game's own window: Windows hands over every frame the game
//!   presents, identical or not, so a running game that delivers none means
//!   the capture broke.
//! - Recording the monitor: the game process's own GPU share decides. Another
//!   process's load (a local LLM, a video) is not the game's and never counts.
//!   The share is read only on detailed samples, which the recorder asks for
//!   while a sample is suspect.

/// Fewer fresh frames per second than this, with a game in front, is suspect.
const FRESH_FPS_SUSPECT: f64 = 5.0;
/// A game process keeping its busiest GPU engine at least this busy is drawing.
const GAME_GPU_DRAWING_PERCENT: f64 = 15.0;

#[derive(Debug, Clone, Copy, PartialEq)]
pub(super) enum Verdict {
    Healthy,
    /// Needs the game's GPU share from a detailed sample to decide.
    Suspect,
    Stale,
}

pub(super) struct Input {
    pub game_in_front: bool,
    pub fresh_fps: f64,
    pub output_fps: f64,
    pub target_fps: f64,
    pub recording_game_window: bool,
    /// None when not read (no detailed sample, or the game refused the query).
    pub game_gpu_percent: Option<f64>,
}

pub(super) fn judge(input: &Input) -> Verdict {
    // The encoder padding to most of the target while capture has nothing
    // new: what makes a frozen recording look healthy.
    let padding = input.output_fps >= input.target_fps * 0.5;
    if !input.game_in_front || input.fresh_fps >= FRESH_FPS_SUSPECT || !padding {
        return Verdict::Healthy;
    }
    if input.recording_game_window {
        return Verdict::Stale;
    }
    match input.game_gpu_percent {
        Some(share) if share >= GAME_GPU_DRAWING_PERCENT => Verdict::Stale,
        Some(_) => Verdict::Healthy,
        None => Verdict::Suspect,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frozen_monitor(game_gpu_percent: Option<f64>) -> Input {
        Input {
            game_in_front: true,
            fresh_fps: 0.0,
            output_fps: 60.0,
            target_fps: 60.0,
            recording_game_window: false,
            game_gpu_percent,
        }
    }

    #[test]
    fn a_game_window_that_delivers_nothing_is_stale_without_more_evidence() {
        let input = Input { recording_game_window: true, ..frozen_monitor(None) };
        assert_eq!(judge(&input), Verdict::Stale);
    }

    #[test]
    fn the_monitor_needs_the_games_own_gpu_share() {
        assert_eq!(judge(&frozen_monitor(None)), Verdict::Suspect);
        assert_eq!(judge(&frozen_monitor(Some(85.0))), Verdict::Stale);
        // A paused or still game, even with an LLM loading the GPU: the
        // share is the game's alone, so a still screen stays healthy.
        assert_eq!(judge(&frozen_monitor(Some(2.0))), Verdict::Healthy);
    }

    #[test]
    fn fresh_frames_no_game_or_no_padding_are_healthy() {
        assert_eq!(judge(&Input { fresh_fps: 59.0, ..frozen_monitor(Some(90.0)) }), Verdict::Healthy);
        assert_eq!(judge(&Input { game_in_front: false, ..frozen_monitor(Some(90.0)) }), Verdict::Healthy);
        assert_eq!(judge(&Input { output_fps: 0.0, ..frozen_monitor(Some(90.0)) }), Verdict::Healthy);
    }
}
