#[cfg(not(windows))]
mod fallback;
#[cfg(windows)]
pub mod windows;

#[cfg(not(windows))]
pub use fallback::NativeIconSource;
#[cfg(windows)]
pub use windows::HostSystemInfo;
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
