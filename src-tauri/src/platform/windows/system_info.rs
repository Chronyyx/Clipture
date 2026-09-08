use crate::diagnostics::{OperatingSystemInfo, SystemInfoProvider};
use windows::{
    Wdk::System::SystemServices::RtlGetVersion,
    Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX, OSVERSIONINFOW},
};

pub struct HostSystemInfo;

impl SystemInfoProvider for HostSystemInfo {
    fn snapshot(&self) -> OperatingSystemInfo {
        let mut memory = MEMORYSTATUSEX {
            dwLength: std::mem::size_of::<MEMORYSTATUSEX>() as u32,
            ..Default::default()
        };
        let mut version = OSVERSIONINFOW {
            dwOSVersionInfoSize: std::mem::size_of::<OSVERSIONINFOW>() as u32,
            ..Default::default()
        };
        // RtlGetVersion is not subject to GetVersionEx manifest compatibility
        // shims. Both calls fill stack-owned, correctly sized Windows structs.
        let memory_ok = unsafe { GlobalMemoryStatusEx(&mut memory) }.is_ok();
        let version_ok = unsafe { RtlGetVersion(&mut version) }.is_ok();
        let release = if version_ok {
            format!(
                "{}.{}.{}",
                version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber
            )
        } else {
            "Unknown".into()
        };
        OperatingSystemInfo {
            platform: "win32".into(),
            version: format!("Windows {release}"),
            release,
            architecture: if cfg!(target_arch = "x86_64") {
                "x64"
            } else {
                std::env::consts::ARCH
            }
            .into(),
            total_memory_bytes: if memory_ok { memory.ullTotalPhys } else { 0 },
            processor: std::env::var("PROCESSOR_IDENTIFIER").unwrap_or_else(|_| "Unknown".into()),
            logical_processors: std::thread::available_parallelism()
                .map(usize::from)
                .unwrap_or(1),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reads_real_windows_version_and_physical_memory_without_spawning_a_process() {
        let info = HostSystemInfo.snapshot();
        assert_eq!(info.platform, "win32");
        assert!(info.total_memory_bytes > 0);
        assert!(info.release.split('.').count() == 3);
        assert!(info.logical_processors > 0);
    }
}
