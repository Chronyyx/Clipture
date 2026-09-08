use std::{
    fs,
    path::{Path, PathBuf},
    sync::Arc,
};

use super::{
    save_audio::AudioLayout, save_inputs::SaveInputs, save_jobs, save_resolution, FfmpegExecutor,
};
use crate::{
    clips::{destination, publish_saved, save_category},
    contracts::{ClipRecord, ClipSettings},
    error::{AppError, AppResult},
    save::SavedClipProcessor,
};

pub struct MediaSaveProcessor {
    executor: Arc<dyn FfmpegExecutor>,
}

impl MediaSaveProcessor {
    pub fn new(executor: Arc<dyn FfmpegExecutor>) -> Self {
        Self { executor }
    }

    fn stitch(
        &self,
        inputs: &SaveInputs,
        clip: &ClipRecord,
        settings: &ClipSettings,
        temporary: &Path,
        target: (u32, u32),
    ) -> AppResult<PathBuf> {
        let dimensions = clip.segment_resolutions.as_deref().unwrap_or_default();
        let audio_layouts = super::segment_audio::layouts(clip, inputs.files.len())?;
        let same_size = clip.segment_audio_tracks.is_some()
            && audio_layouts
                .iter()
                .all(|layout| layout == &clip.audio_tracks)
            && dimensions.len() == inputs.files.len()
            && dimensions
                .iter()
                .all(|value| save_resolution::parse(value) == Some(target));
        let mut manifest = String::new();
        for (index, source) in inputs.files.iter().enumerate() {
            let path = if same_size {
                source.clone()
            } else {
                // Normalize every segment, not just the differently sized ones:
                // the concat demuxer requires matching stream parameters.
                let output = temporary.join(format!("normalized-{index}.mp4"));
                let mut args = save_jobs::input(source);
                super::segment_audio::normalize(
                    &mut args,
                    &clip.audio_tracks,
                    &audio_layouts[index],
                );
                save_jobs::encode_video(
                    &mut args,
                    target,
                    save_resolution::bitrate(target, settings),
                );
                save_jobs::run_output(self.executor.as_ref(), args, &output, true)?;
                output
            };
            manifest.push_str(&concat_line(&path)?);
        }
        let manifest_path = temporary.join("segments.ffconcat");
        fs::write(&manifest_path, manifest).map_err(|source| AppError::Io {
            action: "write private segment manifest",
            path: manifest_path.clone(),
            source,
        })?;
        let mut args = [
            "-hide_banner",
            "-loglevel",
            "error",
            "-nostdin",
            "-y",
            "-protocol_whitelist",
            "file",
            "-f",
            "concat",
            "-safe",
            "0",
            "-i",
        ]
        .map(Into::into)
        .to_vec();
        args.push(manifest_path.into_os_string());
        args.extend(["-map", "0:v:0", "-map", "0:a?", "-c", "copy"].map(Into::into));
        let output = temporary.join("stitched.mp4");
        save_jobs::run_output(self.executor.as_ref(), args, &output, false)?;
        Ok(output)
    }
}

impl SavedClipProcessor for MediaSaveProcessor {
    fn process(&self, mut clip: ClipRecord, settings: &ClipSettings) -> AppResult<ClipRecord> {
        let inputs = SaveInputs::validate(&clip, settings)?;
        let destination = destination(&inputs.root, &clip)?;
        let target = save_resolution::target(&clip, settings).ok_or_else(|| {
            AppError::Path(
                "Engine clip has no valid output resolution; original files preserved".into(),
            )
        })?;
        let temporary = tempfile::Builder::new()
            .prefix(".clipture-save-")
            .tempdir_in(&inputs.root)
            .map_err(|source| AppError::Io {
                action: "stage clip processing",
                path: inputs.root.clone(),
                source,
            })?;
        let mut source = if inputs.segmented {
            self.stitch(&inputs, &clip, settings, temporary.path(), target)?
        } else {
            inputs.files[0].clone()
        };
        let scale = !inputs.segmented && save_resolution::parse(&clip.resolution) != Some(target);
        let audio = AudioLayout::new(&clip.audio_tracks, settings);
        if scale || audio.needs_remap() {
            let output = temporary.path().join("processed.mp4");
            let mut args = save_jobs::input(&source);
            save_jobs::map_audio(&mut args, &audio);
            if scale {
                save_jobs::encode_video(
                    &mut args,
                    target,
                    save_resolution::bitrate(target, settings),
                );
            } else {
                args.extend(["-c:v", "copy"].map(Into::into));
            }
            save_jobs::run_output(self.executor.as_ref(), args, &output, scale)?;
            source = output;
        }
        publish_saved(&source, &destination)?;
        // Only complete publication authorizes removing the engine's source
        // files. Failure earlier leaves every segment available for recovery.
        for input in &inputs.files {
            if let Err(error) = fs::remove_file(input) {
                tracing::warn!(path = %input.display(), %error, "completed clip published but original input cleanup failed");
            }
        }
        clip.file_path = destination.to_string_lossy().into_owned();
        clip.folder_name = Some(save_category(&clip));
        clip.resolution = format!("{}x{}", target.0, target.1);
        clip.audio_tracks = audio.labels;
        clip.segment_files = None;
        clip.segment_resolutions = None;
        clip.segment_audio_tracks = None;
        Ok(clip)
    }
}

fn concat_line(path: &Path) -> AppResult<String> {
    let path = path
        .to_str()
        .ok_or_else(|| AppError::Path("Segment path is not Unicode".into()))?;
    if path.contains(['\n', '\r', '\0']) {
        return Err(AppError::Path(
            "Segment path contains a manifest delimiter".into(),
        ));
    }
    // FFmpeg paths are data inside its own quoting grammar, never shell input.
    let path = if let Some(share) = path.strip_prefix(r"\\?\UNC\") {
        format!("//{}", share.replace('\\', "/"))
    } else {
        path.strip_prefix(r"\\?\")
            .unwrap_or(path)
            .replace('\\', "/")
    };
    let escaped = path.replace('\'', "'\\''");
    Ok(format!("file '{escaped}'\n"))
}

#[cfg(test)]
mod manifest_tests {
    use super::*;
    #[test]
    fn extended_unc_paths_remain_absolute_and_newlines_are_rejected() {
        assert_eq!(
            concat_line(Path::new(r"\\?\UNC\server\clips\one.mp4")).unwrap(),
            "file '//server/clips/one.mp4'\n"
        );
        assert!(concat_line(Path::new("bad\nfile.mp4")).is_err());
    }
}
