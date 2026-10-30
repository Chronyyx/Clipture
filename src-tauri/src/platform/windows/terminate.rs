//! End a child process Clipture owns, by the PID it spawned.
use windows::Win32::{
    Foundation::CloseHandle,
    System::Threading::{OpenProcess, TerminateProcess, PROCESS_TERMINATE},
};

/// Callers pass only the PID of a live child they hold, never a PID found by
/// name or supplied by the renderer.
pub fn terminate_process(pid: u32, exit_code: u32) -> Result<(), windows::core::Error> {
    unsafe {
        let handle = OpenProcess(PROCESS_TERMINATE, false, pid)?;
        let result = TerminateProcess(handle, exit_code);
        let _ = CloseHandle(handle);
        result
    }
}

#[cfg(debug_assertions)]
#[link(name = "ntdll")]
extern "system" {
    fn NtSuspendProcess(process: windows::Win32::Foundation::HANDLE) -> i32;
}

/// Freezes an owned child the way a driver hang freezes it: alive, silent.
/// Debug-only, for the encoder watchdog's hardware smoke test.
#[cfg(debug_assertions)]
pub fn suspend_process_for_smoke(pid: u32) -> Result<(), windows::core::Error> {
    unsafe {
        let access = windows::Win32::System::Threading::PROCESS_SUSPEND_RESUME;
        let handle = OpenProcess(access, false, pid)?;
        let status = NtSuspendProcess(handle);
        let _ = CloseHandle(handle);
        if status < 0 {
            return Err(windows::core::Error::from_hresult(windows::core::HRESULT(status)));
        }
        Ok(())
    }
}
