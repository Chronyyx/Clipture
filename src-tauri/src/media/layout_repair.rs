//! User-started repair of clips whose audio is stored far from its video, or
//! that are largely padding (in-place recordings after the replay buffer
//! wrapped). A lossless FFmpeg remux (`-c copy`) writes an interleaved copy
//! next to the clip; it replaces the original only after the copy is verified.
use std::{
    ffi::OsString,
    fs,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    thread,
    time::Duration,
};

use crate::{
    contracts::{ClipRepairPhase, ClipRepairStatus},
    error::{AppError, AppResult},
};

use super::{
    layout_probe::{is_padded, needs_interleave_repair, needs_stream_layout, read_layout, TrackLayout},
    library_input, FfmpegExecutor, FfmpegJob,
};

const REMUX_TIMEOUT: Duration = Duration::from_secs(30 * 60);
const WORKING_SUFFIX: &str = ".clipture-fixing.mp4";

#[derive(Clone, Debug)]
pub struct RepairCandidate {
    pub title: String,
    pub path: PathBuf,
}

pub struct ClipLayoutRepair {
    ffmpeg: Arc<dyn FfmpegExecutor>,
    replace: fn(&Path, &Path) -> std::io::Result<()>,
    state: Arc<Mutex<RepairState>>,
}

#[derive(Default)]
struct RepairState {
    status: ClipRepairStatus,
    running: bool,
    pending: Vec<RepairCandidate>,
}

impl ClipLayoutRepair {
    pub fn new(ffmpeg: Arc<dyn FfmpegExecutor>) -> Self {
        Self::with_replace(ffmpeg, crate::platform::replace_file)
    }

    fn with_replace(ffmpeg: Arc<dyn FfmpegExecutor>, replace: fn(&Path, &Path) -> std::io::Result<()>) -> Self {
        Self {
            ffmpeg,
            replace,
            state: Arc::new(Mutex::new(RepairState::default())),
        }
    }

    pub fn status(&self) -> ClipRepairStatus {
        self.lock().status.clone()
    }

    /// Reads each clip's index (never its media) and records which need fixing.
    pub fn check(&self, clips: Vec<RepairCandidate>) -> ClipRepairStatus {
        let mut state = self.lock();
        if state.running {
            return state.status.clone();
        }
        state.running = true;
        state.pending.clear();
        state.status = ClipRepairStatus {
            phase: ClipRepairPhase::Checking,
            total: clips.len() as u32,
            ..ClipRepairStatus::default()
        };
        let snapshot = state.status.clone();
        drop(state);
        let shared = Arc::clone(&self.state);
        thread::spawn(move || {
            for clip in clips {
                let bytes = fs::metadata(&clip.path).map(|m| m.len()).unwrap_or(0);
                let needs = read_layout(&clip.path).is_ok_and(|tracks| needs_repair(&tracks, bytes));
                let bytes = if needs { bytes } else { 0 };
                let mut state = shared.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
                state.status.checked += 1;
                if needs {
                    state.status.needs_repair += 1;
                    state.status.needs_repair_bytes += bytes;
                    state.pending.push(clip);
                }
            }
            let mut state = shared.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            state.status.phase = ClipRepairPhase::Checked;
            state.running = false;
        });
        snapshot
    }

    /// Repairs the clips found by the last check, one at a time.
    pub fn repair(&self) -> ClipRepairStatus {
        let mut state = self.lock();
        if state.running || state.status.phase != ClipRepairPhase::Checked || state.pending.is_empty() {
            return state.status.clone();
        }
        state.running = true;
        let clips = std::mem::take(&mut state.pending);
        state.status.phase = ClipRepairPhase::Repairing;
        state.status.message = None;
        let snapshot = state.status.clone();
        drop(state);
        let shared = Arc::clone(&self.state);
        let ffmpeg = Arc::clone(&self.ffmpeg);
        let replace = self.replace;
        thread::spawn(move || {
            let mut last_error = None;
            for clip in clips {
                shared.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).status.current_title = Some(clip.title.clone());
                let result = repair_clip(ffmpeg.as_ref(), replace, &clip.path);
                let mut state = shared.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
                match result {
                    Ok(()) => state.status.repaired += 1,
                    Err(error) => {
                        state.status.failed += 1;
                        last_error = Some(format!("{}: {error}", clip.title));
                    }
                }
            }
            let mut state = shared.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            state.status.phase = ClipRepairPhase::Done;
            state.status.current_title = None;
            state.status.message = last_error;
            state.running = false;
        });
        snapshot
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, RepairState> {
        self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

fn working_path(path: &Path) -> PathBuf {
    let mut name = path.file_stem().map(OsString::from).unwrap_or_default();
    name.push(WORKING_SUFFIX);
    path.with_file_name(name)
}

fn repair_clip(ffmpeg: &dyn FfmpegExecutor, replace: fn(&Path, &Path) -> std::io::Result<()>, path: &Path) -> AppResult<()> {
    let original = read_layout(path)?;
    let bytes = fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    if !needs_repair(&original, bytes) {
        return Ok(()); // Already fine (fixed elsewhere or replaced since the check).
    }
    let modified = fs::metadata(path).and_then(|metadata| metadata.modified()).ok();
    let working = working_path(path);
    let _ = fs::remove_file(&working);
    let result = remux(ffmpeg, path, &working, &[])
        .and_then(|()| verify(&original, &read_layout(&working)?))
        .and_then(|()| {
            replace(path, &working).map_err(|source| AppError::Io {
                action: "replace clip with repaired copy",
                path: path.to_owned(),
                source,
            })
        });
    if result.is_err() {
        let _ = fs::remove_file(&working);
        return result;
    }
    // Keep "Date modified" as the recording time, as users sort by it.
    if let Some(modified) = modified {
        let _ = fs::OpenOptions::new().write(true).open(path).and_then(|file| file.set_modified(modified));
    }
    Ok(())
}

/// Audio far from its video (slow starts, black screens) or a file that is
/// largely padding (wasted space). Out-of-order samples alone are left: they
/// only matter when streaming to a friend, which sends a copy instead.
fn needs_repair(tracks: &[TrackLayout], file_bytes: u64) -> bool {
    needs_interleave_repair(tracks) || is_padded(tracks, file_bytes)
}

/// Writes `output`: a lossless copy of `input` in time order with its index
/// first, verified to hold the same samples, for sending to a friend. Returns
/// `false` (writing nothing) when the clip already streams well as stored.
pub fn write_stream_copy(ffmpeg: &dyn FfmpegExecutor, input: &Path, output: &Path) -> AppResult<bool> {
    let original = read_layout(input)?;
    let size = fs::metadata(input)
        .map_err(|source| AppError::Io {
            action: "inspect clip to share",
            path: input.to_owned(),
            source,
        })?
        .len();
    if !needs_stream_layout(&original, size) {
        return Ok(false);
    }
    let _ = fs::remove_file(output);
    let faststart = [OsString::from("-movflags"), OsString::from("+faststart")];
    let result = remux(ffmpeg, input, output, &faststart)
        .and_then(|()| verify(&original, &read_layout(output)?));
    if let Err(error) = result {
        let _ = fs::remove_file(output);
        return Err(error);
    }
    Ok(true)
}

fn remux(ffmpeg: &dyn FfmpegExecutor, input: &Path, output: &Path, extra: &[OsString]) -> AppResult<()> {
    let head = [
        OsString::from("-nostdin"),
        OsString::from("-hide_banner"),
        OsString::from("-loglevel"),
        OsString::from("error"),
    ];
    let mut job = FfmpegJob::new(
        "clip layout repair",
        head.into_iter().chain(library_input(input)).chain([
            OsString::from("-map"),
            OsString::from("0:v"),
            OsString::from("-map"),
            OsString::from("0:a?"),
            OsString::from("-c"),
            OsString::from("copy"),
            OsString::from("-map_metadata"),
            OsString::from("0"),
        ])
        .chain(extra.iter().cloned())
        .chain([
            OsString::from("-f"),
            OsString::from("mp4"),
            output.as_os_str().to_owned(),
        ]),
    );
    job.timeout = REMUX_TIMEOUT;
    job.maximum_stdout_bytes = 64 * 1024;
    let result = ffmpeg.run(job)?;
    if result.success {
        Ok(())
    } else {
        Err(AppError::Integration(if result.stderr.is_empty() {
            format!("remux exited with code {:?}", result.exit_code)
        } else {
            result.stderr
        }))
    }
}

/// Video must be byte-for-byte the same samples; audio may differ by a few
/// frames where FFmpeg fills a trailing gap, never in length or track order.
fn verify(original: &[TrackLayout], repaired: &[TrackLayout]) -> AppResult<()> {
    let rejected = |reason: &str| Err(AppError::Integration(format!("repaired copy rejected: {reason}")));
    if original.len() != repaired.len() {
        return rejected("track count changed");
    }
    for (before, after) in original.iter().zip(repaired) {
        if before.handler != after.handler {
            return rejected("track order changed");
        }
        if before.is_video() && (before.sizes != after.sizes) {
            return rejected("video samples changed");
        }
        let allowed = (before.sizes.len() / 50).max(64);
        if before.sizes.len().abs_diff(after.sizes.len()) > allowed {
            return rejected("audio samples changed");
        }
        if (before.duration_seconds - after.duration_seconds).abs() > 1.0 {
            return rejected("duration changed");
        }
    }
    if needs_interleave_repair(repaired) {
        return rejected("audio is still far from its video");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::media::{layout_probe::tests::{movie, track_box}, FfmpegOutput};
    use std::time::Instant;

    #[test]
    fn padded_clips_are_offered_for_fixing_but_merely_scrambled_ones_are_not() {
        let tracks = |offsets: &[u32]| {
            crate::media::layout_from_moov(&movie(&[track_box(b"vide", offsets, 1000, 100)])).unwrap()
        };
        let ordered: Vec<u32> = (0..100).map(|i| i * 1000).collect();
        assert!(!needs_repair(&tracks(&ordered), 100_000));
        assert!(needs_repair(&tracks(&ordered), 100_000 + 20 * 1024 * 1024), "half padding");
        let scrambled: Vec<u32> = (0..100).map(|i| ((i * 37) % 100) * 1000).collect();
        assert!(!needs_repair(&tracks(&scrambled), 100_000), "plays fine locally");
    }

    /// `$env:CLIPTURE_STREAM_COPY_CLIP = "C:\...\clip.mp4"`, then
    /// `cargo test --lib real_stream_copy -- --ignored --nocapture`.
    #[test]
    #[ignore = "needs CLIPTURE_STREAM_COPY_CLIP and the bundled FFmpeg"]
    fn real_stream_copy() {
        let input = PathBuf::from(std::env::var("CLIPTURE_STREAM_COPY_CLIP").unwrap());
        let ffmpeg = std::env::current_dir()
            .unwrap()
            .join("binaries/ffmpeg-x86_64-pc-windows-msvc.exe");
        let ffmpeg = crate::media::CommandFfmpeg::new(ffmpeg, Arc::new(crate::media::AlwaysReady));
        let output = input.with_extension("stream-copy.mp4");
        let started = Instant::now();
        let written = write_stream_copy(&ffmpeg, &input, &output).unwrap();
        let before = fs::metadata(&input).unwrap().len();
        println!("copy written: {written} in {:?}", started.elapsed());
        if written {
            let after = fs::metadata(&output).unwrap().len();
            let layout = read_layout(&output).unwrap();
            println!("{} MB -> {} MB; copy needs another: {}", before >> 20, after >> 20,
                needs_stream_layout(&layout, after));
            assert!(!needs_stream_layout(&layout, after));
        }
    }

    fn clip_file(path: &Path, audio_after_video: bool) {
        let video: Vec<u32> = (0..100).map(|i| 64 + i * 4_000_000).collect();
        let audio: Vec<u32> = if audio_after_video {
            (0..100).map(|i| 400_000_064 + i * 10).collect()
        } else {
            video.iter().map(|offset| offset + 1000).collect()
        };
        let index = movie(&[track_box(b"vide", &video, 1000, 100), track_box(b"soun", &audio, 10, 100)]);
        // A 64-byte `free` box stands in for the media before the index.
        let mut bytes = 64_u32.to_be_bytes().to_vec();
        bytes.extend_from_slice(b"free");
        bytes.resize(64, 0);
        bytes.extend(index);
        fs::write(path, bytes).unwrap();
    }

    /// Writes an interleaved (or, when broken, still split) copy to the output path.
    struct FakeRemux {
        broken: bool,
    }
    impl FfmpegExecutor for FakeRemux {
        fn run(&self, job: FfmpegJob) -> AppResult<FfmpegOutput> {
            clip_file(Path::new(job.args.last().unwrap()), self.broken);
            Ok(FfmpegOutput { success: true, exit_code: Some(0), stdout: Vec::new(), stderr: String::new() })
        }
    }

    fn replace(original: &Path, replacement: &Path) -> std::io::Result<()> {
        fs::rename(replacement, original)
    }

    fn wait_for(repair: &ClipLayoutRepair, phase: ClipRepairPhase) -> ClipRepairStatus {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            let status = repair.status();
            if status.phase == phase || Instant::now() > deadline {
                return status;
            }
            thread::sleep(Duration::from_millis(5));
        }
    }

    #[test]
    fn check_then_repair_fixes_only_split_clips() {
        let root = tempfile::tempdir().unwrap();
        let (split, fine) = (root.path().join("old.mp4"), root.path().join("new.mp4"));
        clip_file(&split, true);
        clip_file(&fine, false);
        let repair = ClipLayoutRepair::with_replace(Arc::new(FakeRemux { broken: false }), replace);
        assert_eq!(repair.repair().phase, ClipRepairPhase::Idle, "repair requires a check first");
        repair.check(vec![
            RepairCandidate { title: "old".into(), path: split.clone() },
            RepairCandidate { title: "new".into(), path: fine.clone() },
        ]);
        let checked = wait_for(&repair, ClipRepairPhase::Checked);
        assert_eq!((checked.checked, checked.needs_repair), (2, 1));
        repair.repair();
        let done = wait_for(&repair, ClipRepairPhase::Done);
        assert_eq!((done.repaired, done.failed), (1, 0));
        assert!(!needs_interleave_repair(&read_layout(&split).unwrap()));
        assert!(!working_path(&split).exists());
    }

    /// Manual end-to-end check on a copy of a real clip:
    /// CLIPTURE_REPAIR_SAMPLE=<clip.mp4> CLIPTURE_TEST_FFMPEG=<ffmpeg.exe> cargo test -- --ignored real_clip
    #[test]
    #[ignore]
    fn real_clip_copy_is_repaired_with_the_platform_swap() {
        let sample = PathBuf::from(std::env::var_os("CLIPTURE_REPAIR_SAMPLE").expect("set CLIPTURE_REPAIR_SAMPLE"));
        let ffmpeg = PathBuf::from(std::env::var_os("CLIPTURE_TEST_FFMPEG").expect("set CLIPTURE_TEST_FFMPEG"));
        let root = tempfile::tempdir().unwrap();
        let clip = root.path().join("clip.mp4");
        fs::copy(&sample, &clip).unwrap();
        let created = fs::metadata(&clip).unwrap().created().unwrap();
        let original = read_layout(&clip).unwrap();
        assert!(needs_interleave_repair(&original), "sample must be a split clip");
        let executor = crate::media::CommandFfmpeg::with_concurrency(ffmpeg, Arc::new(crate::media::AlwaysReady), 1);
        let repair = ClipLayoutRepair::new(Arc::new(executor));
        repair.check(vec![RepairCandidate { title: "sample".into(), path: clip.clone() }]);
        assert_eq!(wait_for(&repair, ClipRepairPhase::Checked).needs_repair, 1);
        repair.repair();
        let deadline = Instant::now() + Duration::from_secs(600);
        while repair.status().phase != ClipRepairPhase::Done && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(50));
        }
        let done = repair.status();
        assert_eq!((done.repaired, done.failed), (1, 0), "{:?}", done.message);
        let repaired = read_layout(&clip).unwrap();
        assert!(!needs_interleave_repair(&repaired));
        assert_eq!(original[0].sizes, repaired[0].sizes, "video samples are untouched");
        assert_eq!(fs::metadata(&clip).unwrap().created().unwrap(), created, "creation time survives the swap");
        assert!(!working_path(&clip).exists());
    }

    #[test]
    fn an_unverified_copy_never_replaces_the_original() {
        let root = tempfile::tempdir().unwrap();
        let split = root.path().join("old.mp4");
        clip_file(&split, true);
        let before = fs::read(&split).unwrap();
        let repair = ClipLayoutRepair::with_replace(Arc::new(FakeRemux { broken: true }), replace);
        repair.check(vec![RepairCandidate { title: "old".into(), path: split.clone() }]);
        wait_for(&repair, ClipRepairPhase::Checked);
        repair.repair();
        let done = wait_for(&repair, ClipRepairPhase::Done);
        assert_eq!((done.repaired, done.failed), (0, 1));
        assert!(done.message.unwrap().contains("still far"));
        assert_eq!(fs::read(&split).unwrap(), before);
        assert!(!working_path(&split).exists());
    }
}
