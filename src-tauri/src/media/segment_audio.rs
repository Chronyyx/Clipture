use crate::{
    contracts::ClipRecord,
    error::{AppError, AppResult},
};
use std::{collections::HashSet, ffi::OsString};

pub fn layouts(clip: &ClipRecord, segment_count: usize) -> AppResult<Vec<Vec<String>>> {
    let tracks = &clip.audio_tracks;
    let valid = |names: &[String]| {
        names.len() <= 16
            && names
                .iter()
                .all(|name| !name.is_empty() && name.len() <= 512)
            && names.iter().collect::<HashSet<_>>().len() == names.len()
    };
    if !valid(tracks) {
        return Err(AppError::Path(
            "Invalid saved audio track identities".into(),
        ));
    }
    let layouts = clip
        .segment_audio_tracks
        .clone()
        .unwrap_or_else(|| vec![tracks.clone(); segment_count]);
    if layouts.len() != segment_count
        || layouts
            .iter()
            .any(|layout| !valid(layout) || layout.iter().any(|track| !tracks.contains(track)))
    {
        return Err(AppError::Path(
            "Segment audio layouts do not match the clip contract; original segments preserved"
                .into(),
        ));
    }
    Ok(layouts)
}

/// Normalize every segment to the union's stream order. Audio edit-list delays
/// become leading silence; absent/short tracks are padded to the video span.
/// Names are only lookup keys, never inserted into the filter expression.
pub fn normalize(args: &mut Vec<OsString>, union: &[String], actual: &[String]) {
    args.extend(["-map", "0:v:0"].map(Into::into));
    if union.is_empty() {
        return;
    }
    let filters: Vec<_> = union.iter().enumerate().map(|(output, name)| {
        match actual.iter().position(|candidate| candidate == name) {
            Some(input) => format!("[0:a:{input}]aresample=48000:async=1:first_pts=0,aformat=channel_layouts=stereo,apad[track{output}]"),
            None => format!("anullsrc=r=48000:cl=stereo[track{output}]"),
        }
    }).collect();
    args.extend(["-filter_complex".into(), filters.join(";").into()]);
    for output in 0..union.len() {
        args.extend(["-map".into(), format!("[track{output}]").into()]);
    }
    args.extend(
        [
            "-shortest",
            "-shortest_buf_duration",
            "2",
            "-c:a",
            "aac",
            "-b:a",
            "192k",
        ]
        .map(Into::into),
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn remaps_by_identity_and_supplies_missing_stream_without_filter_injection() {
        let mut args = vec![];
        normalize(
            &mut args,
            &["mic".into(), "app:[];evil".into()],
            &["app:[];evil".into()],
        );
        let filter = args[args
            .iter()
            .position(|arg| arg == "-filter_complex")
            .unwrap()
            + 1]
        .to_string_lossy();
        assert!(filter.contains("anullsrc=r=48000:cl=stereo[track0]"));
        assert!(filter.contains("[0:a:0]aresample"));
        assert!(!filter.contains("evil"));
    }
    #[test]
    fn rejects_inconsistent_metadata() {
        let mut clip = ClipRecord {
            audio_tracks: vec!["mic".into()],
            segment_audio_tracks: Some(vec![vec!["unknown".into()]]),
            ..ClipRecord::default()
        };
        assert!(layouts(&clip, 1).is_err());
        clip.segment_audio_tracks = Some(vec![vec!["mic".into(), "mic".into()]]);
        assert!(layouts(&clip, 1).is_err());
        clip.segment_audio_tracks = Some(vec![vec![]]);
        assert!(layouts(&clip, 2).is_err());
        assert_eq!(layouts(&clip, 1).unwrap(), vec![Vec::<String>::new()]);
    }
}
