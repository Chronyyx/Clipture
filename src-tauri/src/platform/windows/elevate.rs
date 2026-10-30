//! Runs a helper elevated behind Windows' administrator prompt, and names the
//! signed-in user so an elevated helper can refuse to act for anyone else.
use std::path::Path;

/// `Ok(None)` when the prompt was declined, else the helper's exit code once
/// it finishes (at most five minutes).
pub(crate) fn run_elevated(helper: &Path, arguments: &str) -> Result<Option<u32>, String> {
    use std::os::windows::ffi::OsStrExt;
    use windows::{
        core::PCWSTR,
        Win32::{
            Foundation::{CloseHandle, WAIT_OBJECT_0},
            System::Threading::{GetExitCodeProcess, WaitForSingleObject},
            UI::{
                Shell::{ShellExecuteExW, SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW},
                WindowsAndMessaging::SW_HIDE,
            },
        },
    };
    let wide = |text: &std::ffi::OsStr| text.encode_wide().chain(Some(0)).collect::<Vec<u16>>();
    let (verb, file, parameters) = (wide("runas".as_ref()), wide(helper.as_os_str()), wide(arguments.as_ref()));
    let mut info = SHELLEXECUTEINFOW {
        cbSize: std::mem::size_of::<SHELLEXECUTEINFOW>() as u32,
        fMask: SEE_MASK_NOCLOSEPROCESS,
        lpVerb: PCWSTR(verb.as_ptr()),
        lpFile: PCWSTR(file.as_ptr()),
        lpParameters: PCWSTR(parameters.as_ptr()),
        nShow: SW_HIDE.0,
        ..Default::default()
    };
    // SAFETY: every string outlives the call; the process handle is closed below.
    if let Err(error) = unsafe { ShellExecuteExW(&mut info) } {
        const CANCELLED: i32 = 1223;
        return if error.code().0 & 0xFFFF == CANCELLED { Ok(None) } else { Err(error.to_string()) };
    }
    let process = info.hProcess;
    let mut code = 1_u32;
    // SAFETY: `process` is the owned handle ShellExecuteExW returned.
    let finished = unsafe {
        let waited = WaitForSingleObject(process, 5 * 60 * 1000) == WAIT_OBJECT_0;
        let read = waited && GetExitCodeProcess(process, &mut code).is_ok();
        let _ = CloseHandle(process);
        read
    };
    if finished { Ok(Some(code)) } else { Err("The elevated helper did not finish".into()) }
}

/// The signed-in user's SID (`S-1-5-21-...`).
pub(crate) fn current_user_sid() -> Option<String> {
    use windows::{
        core::PWSTR,
        Win32::{
            Foundation::{CloseHandle, HANDLE, HLOCAL, LocalFree},
            Security::{Authorization::ConvertSidToStringSidW, GetTokenInformation, TokenUser, TOKEN_QUERY, TOKEN_USER},
            System::Threading::{GetCurrentProcess, OpenProcessToken},
        },
    };
    // SAFETY: the token handle and the SID string are released below; the
    // buffer outlives every pointer into it.
    unsafe {
        let mut token = HANDLE::default();
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token).ok()?;
        let mut size = 0_u32;
        let _ = GetTokenInformation(token, TokenUser, None, 0, &mut size);
        let mut buffer = vec![0_u8; size as usize];
        let read = GetTokenInformation(token, TokenUser, Some(buffer.as_mut_ptr().cast()), size, &mut size);
        let _ = CloseHandle(token);
        read.ok()?;
        let user = &*(buffer.as_ptr() as *const TOKEN_USER);
        let mut text = PWSTR::null();
        ConvertSidToStringSidW(user.User.Sid, &mut text).ok()?;
        let sid = text.to_string().ok();
        let _ = LocalFree(Some(HLOCAL(text.0.cast())));
        sid
    }
}
