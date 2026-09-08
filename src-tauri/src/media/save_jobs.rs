use std::{ffi::OsString, path::Path, time::Duration};

use super::{save_audio::AudioLayout, save_resolution, FfmpegExecutor, FfmpegJob};
use crate::error::{AppError, AppResult};

pub fn input(path: &Path) -> Vec<OsString> {
    let mut args: Vec<OsString> = [
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-y",
        "-threads",
        "2",
        "-protocol_whitelist",
        "file",
        "-f",
        "mov",
        "-i",
    ]
    .map(Into::into)
    .into();
    args.push(path.as_os_str().into());
    args
}

pub fn encode_video(args: &mut Vec<OsString>, target: (u32, u32), bitrate: u32) {
    args.extend([
        "-vf".into(),
        save_resolution::scale(target).into(),
        "-c:v".into(),
        "h264_nvenc".into(),
        "-preset".into(),
        "p5".into(),
        "-b:v".into(),
        format!("{bitrate}M").into(),
        "-pix_fmt".into(),
        "yuv420p".into(),
    ]);
}

pub fn map_audio(args: &mut Vec<OsString>, layout: &AudioLayout) {
    let mut filters = Vec::new();
    for (output, group) in layout.groups.iter().enumerate() {
        if group.len() > 1 {
            let inputs: String = group.iter().map(|index| format!("[0:a:{index}]")).collect();
            filters.push(format!(
                "{inputs}amix=inputs={}:duration=longest:dropout_transition=0[mix{output}]",
                group.len()
            ));
        }
    }
    if !filters.is_empty() {
        args.extend(["-filter_complex".into(), filters.join(";").into()]);
    }
    args.extend(["-map".into(), "0:v:0".into()]);
    for (output, group) in layout.groups.iter().enumerate() {
        let mapping = if group.len() > 1 {
            format!("[mix{output}]")
        } else {
            format!("0:a:{}", group[0])
        };
        args.extend(["-map".into(), mapping.into()]);
    }
    if layout.needs_mix() {
        args.extend(["-c:a", "aac", "-b:a", "192k"].map(Into::into));
    } else {
        args.extend(["-c:a", "copy"].map(Into::into));
    }
}

/// Sequential, bounded jobs keep segment count from multiplying decoder RAM.
/// A CPU fallback is explicit and bounded to two encoder threads; it is used
/// only if the requested NVENC transcode fails (for example driver recovery).
pub fn run_output(
    executor: &dyn FfmpegExecutor,
    mut args: Vec<OsString>,
    output: &Path,
    transcode: bool,
) -> AppResult<()> {
    args.extend(
        [
            "-threads",
            "2",
            "-filter_threads",
            "2",
            "-filter_complex_threads",
            "2",
            "-movflags",
            "+faststart",
            "-f",
            "mp4",
        ]
        .map(Into::into),
    );
    args.push(output.as_os_str().into());
    let run = |args: Vec<OsString>| {
        let mut job = FfmpegJob::new("finalize saved clip", args);
        job.timeout = Duration::from_secs(180);
        job.maximum_stdout_bytes = 1024;
        executor.run(job)
    };
    let mut result = run(args.clone())?;
    if !result.success && transcode {
        tracing::warn!(error = %result.stderr, "NVENC clip processing failed; retrying with bounded software encoder");
        for index in 1..args.len() {
            if args[index - 1] == "-c:v" {
                args[index] = "libx264".into();
            }
            if args[index - 1] == "-preset" {
                args[index] = "veryfast".into();
            }
        }
        result = run(args)?;
    }
    if !result.success {
        return Err(AppError::Integration(format!(
            "Clip processing failed; original files preserved: {}",
            result.stderr
        )));
    }
    let metadata = std::fs::metadata(output).map_err(|source| AppError::Io {
        action: "inspect processed clip",
        path: output.into(),
        source,
    })?;
    if !metadata.is_file() || metadata.len() < 32 {
        return Err(AppError::Integration(
            "Clip processing produced an empty file; original files preserved".into(),
        ));
    }
    Ok(())
}
