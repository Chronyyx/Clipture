#[cfg(not(windows))]
mod fallback;
#[cfg(windows)]
pub mod windows;

#[cfg(not(windows))]
pub use fallback::NativeIconSource;
#[cfg(windows)]
pub use windows::HostSystemInfo;
#[cfg(windows)]
pub(crate) use windows::replace_file;
#[cfg(windows)]
pub(crate) use windows::detect_default_audio_apps;

#[cfg(not(windows))]
pub(crate) fn detect_default_audio_apps() -> Vec<crate::settings::DetectedAudioApp> {
    Vec::new()
}

/// Portable fallback: rename over the original (loses its creation time).
#[cfg(not(windows))]
pub(crate) fn replace_file(original: &std::path::Path, replacement: &std::path::Path) -> std::io::Result<()> {
    std::fs::rename(replacement, original)
}
#[cfg(windows)]
pub use windows::WindowsIconSource as NativeIconSource;

#[cfg(not(windows))]
pub struct HostSystemInfo;
#[cfg(not(windows))]
impl crate::diagnostics::SystemInfoProvider for HostSystemInfo {
    fn snapshot(&self) -> crate::diagnostics::OperatingSystemInfo {
        crate::diagnostics::OperatingSystemInfo {
            platform: std::env::consts::OS.into(),
            architecture: std::env::consts::ARCH.into(),
            logical_processors: std::thread::available_parallelism()
                .map(usize::from)
                .unwrap_or(1),
            ..Default::default()
        }
    }
}
