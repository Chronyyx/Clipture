//! Ending processes: a child Clipture owns, by the PID it spawned, and
//! telling when Windows itself is ending them at shutdown or sign-out.
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

/// The exit code Windows gives a process it ends at shutdown or sign-out
/// (`DBG_TERMINATE_PROCESS`).
pub const ENDED_BY_WINDOWS: u32 = 0x4001_0004;

/// Windows is ending the session (shutdown, restart or sign-out); a process
/// that dies now was ended by Windows, not by a Clipture bug.
pub fn session_ending() -> bool {
    use windows::Win32::UI::WindowsAndMessaging::{GetSystemMetrics, SM_SHUTTINGDOWN};
    unsafe { GetSystemMetrics(SM_SHUTTINGDOWN) != 0 }
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

#[cfg(test)]
mod tests {
    #[test]
    fn a_running_session_is_not_ending() {
        assert!(!super::session_ending());
    }
}
