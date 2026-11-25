//! Carries a monitor choice saved as a Windows output name (`\.\DISPLAY2`)
//! over to the physical monitor's id. Windows renumbers outputs after reboots,
//! driver updates or a monitor waking from sleep, so the old name could point
//! at another screen; the engine's `monitor:` id names the monitor itself.

use crate::contracts::{ClipSettings, DisplayDevice};

const STABLE_PREFIX: &str = "monitor:";

/// The settings with `monitor_id` replaced by the physical monitor's id, or
/// `None` when nothing needs carrying over (already stable, "primary", or the
/// old name matches no connected monitor yet).
pub fn adopt_stable_monitor_id(
    settings: &ClipSettings,
    displays: &[DisplayDevice],
) -> Option<ClipSettings> {
    let saved = settings.monitor_id.trim();
    if saved.is_empty() || saved.eq_ignore_ascii_case("primary") || saved.starts_with(STABLE_PREFIX) {
        return None;
    }
    let display = displays.iter().find(|display| {
        display.id.starts_with(STABLE_PREFIX)
            && display
                .legacy_id
                .as_deref()
                .is_some_and(|legacy| legacy.eq_ignore_ascii_case(saved))
    })?;
    let mut adopted = settings.clone();
    adopted.monitor_id = display.id.clone();
    Some(adopted)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn display(id: &str, legacy: &str) -> DisplayDevice {
        DisplayDevice {
            id: id.into(),
            legacy_id: Some(legacy.into()),
            ..DisplayDevice::default()
        }
    }

    fn with_monitor(id: &str) -> ClipSettings {
        ClipSettings {
            monitor_id: id.into(),
            ..ClipSettings::default()
        }
    }

    const MAIN: &str = r"monitor:\?\display#aus27af#5&1&0&uid4352";
    const SIDE: &str = r"monitor:\?\display#gsm5b7f#5&2&0&uid4356";

    #[test]
    fn an_output_name_becomes_the_monitor_it_names_now() {
        let displays = [display(SIDE, r"\.\DISPLAY1"), display(MAIN, r"\.\DISPLAY2")];
        let adopted = adopt_stable_monitor_id(&with_monitor(r"\.\display2"), &displays).unwrap();
        assert_eq!(adopted.monitor_id, MAIN);
    }

    #[test]
    fn stable_primary_and_unmatched_choices_are_left_alone() {
        let displays = [display(MAIN, r"\.\DISPLAY2")];
        assert!(adopt_stable_monitor_id(&with_monitor(MAIN), &displays).is_none());
        assert!(adopt_stable_monitor_id(&with_monitor("primary"), &displays).is_none());
        assert!(adopt_stable_monitor_id(&with_monitor(r"\.\DISPLAY9"), &displays).is_none());
        // An engine that could not read the device path keeps the output name
        // as the id: nothing better to carry it over to.
        let fallback = [display(r"\.\DISPLAY2", r"\.\DISPLAY2")];
        assert!(adopt_stable_monitor_id(&with_monitor(r"\.\DISPLAY2"), &fallback).is_none());
    }
}
