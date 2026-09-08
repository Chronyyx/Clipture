use std::{
    ffi::{OsStr, OsString},
    io::{Read, Take},
    path::{Path, PathBuf},
    process::{Command, ExitStatus, Stdio},
    sync::{Arc, Condvar, Mutex},
    thread,
    time::{Duration, Instant},
};

use crate::error::{AppError, AppResult};

pub trait WorkScheduler: Send + Sync {
    /// Gives capture pressure a chance to defer disk-heavy background work.
    fn wait_until_ready(&self, maximum_wait: Duration) -> bool;
}

#[derive(Default)]
pub struct AlwaysReady;

impl WorkScheduler for AlwaysReady {
    fn wait_until_ready(&self, _: Duration) -> bool {
        true
    }
}

#[derive(Clone, Debug)]
pub struct FfmpegJob {
    pub label: &'static str,
    pub args: Vec<OsString>,
    pub timeout: Duration,
    pub maximum_stdout_bytes: usize,
}

impl FfmpegJob {
    pub fn new(label: &'static str, args: impl IntoIterator<Item = impl Into<OsString>>) -> Self {
        Self {
            label,
            args: args.into_iter().map(Into::into).collect(),
            timeout: Duration::from_secs(15),
            maximum_stdout_bytes: 2 * 1024 * 1024,
        }
    }
}

#[derive(Clone, Debug)]
pub struct FfmpegOutput {
    pub success: bool,
    pub exit_code: Option<i32>,
    pub stdout: Vec<u8>,
    pub stderr: String,
}

pub trait FfmpegExecutor: Send + Sync {
    fn run(&self, job: FfmpegJob) -> AppResult<FfmpegOutput>;
}

pub struct CommandFfmpeg {
    executable: PathBuf,
    scheduler: Arc<dyn WorkScheduler>,
    limiter: Arc<JobLimiter>,
}

impl CommandFfmpeg {
    pub fn new(executable: impl Into<PathBuf>, scheduler: Arc<dyn WorkScheduler>) -> Self {
        Self::with_concurrency(executable, scheduler, 2)
    }

    pub fn with_concurrency(
        executable: impl Into<PathBuf>,
        scheduler: Arc<dyn WorkScheduler>,
        maximum_jobs: usize,
    ) -> Self {
        Self {
            executable: executable.into(),
            scheduler,
            limiter: Arc::new(JobLimiter::new(maximum_jobs.max(1))),
        }
    }

    pub fn executable(&self) -> &Path {
        &self.executable
    }
}

impl FfmpegExecutor for CommandFfmpeg {
    fn run(&self, job: FfmpegJob) -> AppResult<FfmpegOutput> {
        validate_job(&job)?;
        if !self.executable.is_file() {
            return Err(AppError::Path(format!(
                "FFmpeg executable was not found: {}",
                self.executable.display()
            )));
        }
        if !self.scheduler.wait_until_ready(Duration::from_secs(8)) {
            return Err(AppError::Integration(format!(
                "{} deferred because capture remained under pressure",
                job.label
            )));
        }
        let _permit = self.limiter.acquire();
        let mut command = Command::new(&self.executable);
        command
            .args(&job.args)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        configure_background_process(&mut command);
        let mut child = command.spawn().map_err(|source| AppError::Io {
            action: "start FFmpeg background job",
            path: self.executable.clone(),
            source,
        })?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| AppError::Integration("FFmpeg stdout pipe is unavailable".into()))?;
        let stderr = child
            .stderr
            .take()
            .ok_or_else(|| AppError::Integration("FFmpeg stderr pipe is unavailable".into()))?;
        let stdout_limit = job.maximum_stdout_bytes;
        let stdout_reader = thread::spawn(move || read_bounded(stdout, stdout_limit));
        let stderr_reader = thread::spawn(move || read_bounded(stderr, 16 * 1024));

        let deadline = Instant::now() + job.timeout;
        let status = loop {
            match child.try_wait() {
                Ok(Some(status)) => break status,
                Ok(None) if Instant::now() < deadline => thread::sleep(Duration::from_millis(15)),
                Ok(None) => {
                    let _ = child.kill();
                    let _ = child.wait();
                    let _ = stdout_reader.join();
                    let _ = stderr_reader.join();
                    return Err(AppError::Integration(format!(
                        "{} timed out after {} seconds",
                        job.label,
                        job.timeout.as_secs()
                    )));
                }
                Err(source) => {
                    let _ = child.kill();
                    let _ = child.wait();
                    let _ = stdout_reader.join();
                    let _ = stderr_reader.join();
                    return Err(AppError::Io {
                        action: "wait for FFmpeg background job",
                        path: self.executable.clone(),
                        source,
                    });
                }
            }
        };
        let stdout = join_reader(stdout_reader, "stdout")?;
        let stderr = join_reader(stderr_reader, "stderr")?;
        if stdout.exceeded {
            return Err(AppError::Integration(format!(
                "{} output exceeded {} bytes",
                job.label, job.maximum_stdout_bytes
            )));
        }
        Ok(output(status, stdout.bytes, stderr.bytes))
    }
}

fn validate_job(job: &FfmpegJob) -> AppResult<()> {
    if job.args.len() > 128
        || job
            .args
            .iter()
            .map(|arg| arg.to_string_lossy().len())
            .sum::<usize>()
            > 64 * 1024
    {
        return Err(AppError::Integration(
            "FFmpeg argument list is too large".into(),
        ));
    }
    if job.maximum_stdout_bytes == 0 || job.maximum_stdout_bytes > 64 * 1024 * 1024 {
        return Err(AppError::Integration(
            "FFmpeg output limit is invalid".into(),
        ));
    }
    Ok(())
}

struct CapturedOutput {
    bytes: Vec<u8>,
    exceeded: bool,
}

fn read_bounded(mut reader: impl Read, limit: usize) -> std::io::Result<CapturedOutput> {
    let mut bytes = Vec::with_capacity(limit.min(64 * 1024));
    let mut exceeded = false;
    let mut chunk = [0_u8; 16 * 1024];
    loop {
        let count = reader.read(&mut chunk)?;
        if count == 0 {
            break;
        }
        let remaining = limit.saturating_sub(bytes.len());
        bytes.extend_from_slice(&chunk[..count.min(remaining)]);
        exceeded |= count > remaining;
        // Keep draining after the cap so FFmpeg cannot block on a full pipe.
    }
    Ok(CapturedOutput { bytes, exceeded })
}

fn join_reader(
    reader: thread::JoinHandle<std::io::Result<CapturedOutput>>,
    name: &str,
) -> AppResult<CapturedOutput> {
    reader
        .join()
        .map_err(|_| AppError::Integration(format!("FFmpeg {name} reader panicked")))?
        .map_err(|source| AppError::Io {
            action: "read FFmpeg output",
            path: PathBuf::from(name),
            source,
        })
}

fn output(status: ExitStatus, stdout: Vec<u8>, stderr: Vec<u8>) -> FfmpegOutput {
    FfmpegOutput {
        success: status.success(),
        exit_code: status.code(),
        stdout,
        stderr: String::from_utf8_lossy(&stderr).trim().to_owned(),
    }
}

#[cfg(windows)]
fn configure_background_process(command: &mut Command) {
    use std::os::windows::process::CommandExt;
    const BELOW_NORMAL_PRIORITY_CLASS: u32 = 0x0000_4000;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    command.creation_flags(BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW);
}

#[cfg(not(windows))]
fn configure_background_process(_: &mut Command) {}

struct JobLimiter {
    maximum: usize,
    active: Mutex<usize>,
    changed: Condvar,
}

impl JobLimiter {
    fn new(maximum: usize) -> Self {
        Self {
            maximum,
            active: Mutex::new(0),
            changed: Condvar::new(),
        }
    }

    fn acquire(self: &Arc<Self>) -> JobPermit {
        let mut active = self
            .active
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        while *active >= self.maximum {
            active = self
                .changed
                .wait(active)
                .unwrap_or_else(|poisoned| poisoned.into_inner());
        }
        *active += 1;
        JobPermit(Arc::clone(self))
    }
}

struct JobPermit(Arc<JobLimiter>);

impl Drop for JobPermit {
    fn drop(&mut self) {
        let mut active = self
            .0
            .active
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        *active = active.saturating_sub(1);
        self.0.changed.notify_one();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_unbounded_jobs_before_starting_a_process() {
        let executor = CommandFfmpeg::new("missing-ffmpeg", Arc::new(AlwaysReady));
        let mut job = FfmpegJob::new("bad", [OsStr::new("-version")]);
        job.maximum_stdout_bytes = usize::MAX;
        assert!(executor
            .run(job)
            .unwrap_err()
            .to_string()
            .contains("output limit"));
    }
}
