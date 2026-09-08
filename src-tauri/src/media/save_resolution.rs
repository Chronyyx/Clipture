use crate::contracts::{ClipRecord, ClipSettings};

pub fn parse(value: &str) -> Option<(u32, u32)> {
    let (width, height) = value.split_once('x')?;
    let size = (width.parse::<u32>().ok()?, height.parse::<u32>().ok()?);
    (size.0 >= 2
        && size.1 >= 2
        && size.0 <= 8192
        && size.1 <= 8192
        && size.0 % 2 == 0
        && size.1 % 2 == 0)
        .then_some(size)
}

pub fn target(clip: &ClipRecord, settings: &ClipSettings) -> Option<(u32, u32)> {
    let preset = settings.resolution_preset.size();
    (preset.0 > 0 && preset.1 > 0)
        .then_some(preset)
        .or_else(|| parse(&clip.resolution))
        .or_else(|| clip.recommended_resolution.as_deref().and_then(parse))
}

pub fn bitrate(size: (u32, u32), settings: &ClipSettings) -> u32 {
    if !settings.auto_bitrate {
        return settings.bitrate_mbps.clamp(4, 120);
    }
    let suggested = (24.0 * f64::from(size.0) * f64::from(size.1) / (1920.0 * 1080.0)
        * f64::from(settings.fps.max(1))
        / 30.0)
        .round() as u32;
    suggested.min(settings.max_auto_bitrate_mbps).clamp(4, 120)
}

pub fn scale((width, height): (u32, u32)) -> String {
    format!("scale={width}:{height}:force_original_aspect_ratio=decrease:force_divisible_by=2,pad={width}:{height}:(ow-iw)/2:(oh-ih)/2,setsar=1")
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn malformed_or_unbounded_dimensions_are_not_filter_input() {
        for value in [
            "0x0",
            "999999x1080",
            "1920x1080;movie=evil",
            "1x3",
            "1920X1080",
        ] {
            assert_eq!(parse(value), None);
        }
        assert_eq!(parse("1920x1080"), Some((1920, 1080)));
    }
}
