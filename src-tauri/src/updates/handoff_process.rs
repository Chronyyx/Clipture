use std::{io::{Read, Write}, process::{Child, Command, Stdio}, time::{Duration, Instant}};

pub struct OwnedProcess {
    pub child: Child,
    released: bool,
    #[cfg(windows)]
    job: Option<crate::platform::windows::ChildJob>,
}

impl OwnedProcess {
    pub fn spawn(command: &mut Command) -> std::io::Result<Self> {
        let child = command.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null()).spawn()?;
        let mut owned = Self { child, released: false, #[cfg(windows)] job: None };
        // Handoff children wait on their private activation pipe before they
        // create descendants, so attachment precedes engine/UI startup.
        #[cfg(windows)]
        { owned.job = Some(crate::platform::windows::ChildJob::attach(&owned.child).map_err(std::io::Error::other)?); }
        Ok(owned)
    }

    pub fn send(&mut self, bytes: &[u8]) -> std::io::Result<()> {
        self.child.stdin.as_mut().ok_or_else(|| std::io::Error::other("Missing control pipe"))?.write_all(bytes)
    }

    pub fn expect(&mut self, expected: &'static [u8], timeout: Duration) -> std::io::Result<()> {
        let mut stdout = self.child.stdout.take().ok_or_else(|| std::io::Error::other("Missing health pipe"))?;
        let (sender, receiver) = std::sync::mpsc::sync_channel(1);
        std::thread::spawn(move || {
            let mut message = vec![0; expected.len()];
            let result = stdout.read_exact(&mut message).and_then(|_| {
                if message == expected { Ok(stdout) } else { Err(std::io::Error::other("Invalid process health response")) }
            });
            let _ = sender.send(result);
        });
        self.child.stdout = Some(receiver.recv_timeout(timeout).map_err(|_| std::io::Error::other("Process health response timed out"))??);
        if self.child.try_wait()?.is_some() { return Err(std::io::Error::other("Process exited during handshake")); }
        Ok(())
    }

    pub fn healthy_for(&mut self, duration: Duration) -> std::io::Result<()> {
        let deadline = Instant::now() + duration;
        while Instant::now() < deadline {
            if self.child.try_wait()?.is_some() { return Err(std::io::Error::other("Process exited during health validation")); }
            std::thread::sleep(Duration::from_millis(25));
        }
        Ok(())
    }

    pub fn release(mut self) -> std::io::Result<()> {
        #[cfg(windows)]
        if let Some(job) = &self.job { job.disarm().map_err(std::io::Error::other)?; }
        self.released = true;
        Ok(())
    }
}

impl Drop for OwnedProcess {
    fn drop(&mut self) {
        if !self.released {
            #[cfg(windows)]
            drop(self.job.take());
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}

#[cfg(all(test, windows))]
mod tests {
    use super::*;

    fn fixture(body: &str) -> Command {
        use base64::Engine;
        use std::os::windows::process::CommandExt;
        let bytes: Vec<u8> = body.encode_utf16().flat_map(u16::to_le_bytes).collect();
        let mut command = Command::new("powershell.exe");
        command.args(["-NoProfile", "-NonInteractive", "-EncodedCommand",
            &base64::engine::general_purpose::STANDARD.encode(bytes)]);
        command.creation_flags(0x0800_0000);
        command
    }

    #[test]
    fn owned_child_handshake_health_and_cleanup_use_real_pipes() {
        let mut child = OwnedProcess::spawn(&mut fixture(
            "$o=[Console]::OpenStandardOutput(); $i=[Console]::OpenStandardInput(); $b=[Text.Encoding]::UTF8.GetBytes(\"prepared`n\"); $o.Write($b,0,$b.Length); $o.Flush(); $r=New-Object byte[] 9; $n=0; while($n -lt 9){$c=$i.Read($r,$n,9-$n); if($c -eq 0){exit 4}; $n+=$c}; if([Text.Encoding]::UTF8.GetString($r) -ne \"activate`n\"){exit 5}; $b=[Text.Encoding]::UTF8.GetBytes(\"healthy`n\"); $o.Write($b,0,$b.Length); $o.Flush(); $i.ReadByte() | Out-Null"
        )).unwrap();
        let process = ParentProcess::open(child.child.id()).unwrap();
        child.expect(b"prepared\n", Duration::from_secs(15)).unwrap();
        child.send(b"activate\n").unwrap();
        child.expect(b"healthy\n", Duration::from_secs(15)).unwrap();
        child.healthy_for(Duration::from_millis(100)).unwrap();
        drop(child);
        process.wait(Duration::from_secs(5)).unwrap();
    }

    #[test]
    fn invalid_health_and_timeout_reap_only_owned_child() {
        for script in [
            "$o=[Console]::OpenStandardOutput(); $b=[Text.Encoding]::UTF8.GetBytes(\"invalid!\"); $o.Write($b,0,$b.Length); $o.Flush(); [Console]::OpenStandardInput().ReadByte() | Out-Null",
            "[Console]::OpenStandardInput().ReadByte() | Out-Null",
        ] {
            let mut child = OwnedProcess::spawn(&mut fixture(script)).unwrap();
            let process = ParentProcess::open(child.child.id()).unwrap();
            assert!(child.expect(b"healthy\n", Duration::from_secs(2)).is_err());
            drop(child);
            process.wait(Duration::from_secs(5)).unwrap();
        }
    }

    #[test]
    fn failed_candidate_reaps_descendants_but_not_unrelated_processes() {
        let directory = tempfile::tempdir().unwrap();
        let pid_path = directory.path().join("owned-child.pid");
        let script = format!(
            "$i=[Console]::OpenStandardInput(); $i.ReadByte() | Out-Null; \
             $s=New-Object Diagnostics.ProcessStartInfo; $s.FileName='cmd.exe'; $s.Arguments='/D /Q'; \
             $s.UseShellExecute=$false; $s.CreateNoWindow=$true; $s.RedirectStandardInput=$true; \
             $s.RedirectStandardOutput=$true; $p=[Diagnostics.Process]::Start($s); \
             [IO.File]::WriteAllText('{}',[string]$p.Id); \
             $o=[Console]::OpenStandardOutput(); $b=[Text.Encoding]::UTF8.GetBytes(\"healthy`n\"); \
             $o.Write($b,0,$b.Length); $o.Flush(); $i.ReadByte() | Out-Null; $p.Kill(); $p.WaitForExit()",
            pid_path.to_string_lossy().replace('\'', "''")
        );
        let mut sibling = OwnedProcess::spawn(&mut fixture("[Console]::OpenStandardInput().ReadByte() | Out-Null")).unwrap();
        let mut child = OwnedProcess::spawn(&mut fixture(&script)).unwrap();
        child.send(b"x").unwrap();
        child.expect(b"healthy\n", Duration::from_secs(15)).unwrap();
        let descendant = ParentProcess::open(std::fs::read_to_string(pid_path).unwrap().parse().unwrap()).unwrap();
        drop(child);
        descendant.wait(Duration::from_secs(5)).unwrap();
        assert!(sibling.child.try_wait().unwrap().is_none());
    }

    #[test]
    fn releasing_healthy_process_disarms_kill_on_close() {
        let mut child = OwnedProcess::spawn(&mut fixture(
            "$o=[Console]::OpenStandardOutput(); $b=[Text.Encoding]::UTF8.GetBytes(\"healthy`n\"); $o.Write($b,0,$b.Length); $o.Flush(); [Console]::OpenStandardInput().ReadByte() | Out-Null"
        )).unwrap();
        child.expect(b"healthy\n", Duration::from_secs(15)).unwrap();
        let process = ParentProcess::open(child.child.id()).unwrap();
        let mut input = child.child.stdin.take().unwrap();
        child.release().unwrap();
        assert!(process.wait(Duration::from_millis(50)).is_err());
        input.write_all(b"x").unwrap();
        process.wait(Duration::from_secs(5)).unwrap();
    }
}

#[cfg(windows)]
pub fn input() -> std::io::Result<std::fs::File> { standard_handle(-10_i32 as u32) }
#[cfg(windows)]
pub fn output() -> std::io::Result<std::fs::File> { standard_handle(-11_i32 as u32) }

#[cfg(windows)]
fn standard_handle(which: u32) -> std::io::Result<std::fs::File> {
    use std::os::windows::io::FromRawHandle;
    unsafe extern "system" {
        fn GetStdHandle(which: u32) -> *mut std::ffi::c_void;
        fn GetCurrentProcess() -> *mut std::ffi::c_void;
        fn DuplicateHandle(source_process: *mut std::ffi::c_void, source: *mut std::ffi::c_void,
            target_process: *mut std::ffi::c_void, target: *mut *mut std::ffi::c_void,
            access: u32, inherit: i32, options: u32) -> i32;
    }
    let handle = unsafe { GetStdHandle(which) };
    if handle.is_null() || handle as isize == -1 { return Err(std::io::Error::other("Missing inherited process pipe")); }
    let mut duplicate = std::ptr::null_mut();
    if unsafe { DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &mut duplicate, 0, 0, 2) } == 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(unsafe { std::fs::File::from_raw_handle(duplicate) })
}

#[cfg(not(windows))]
pub fn input() -> std::io::Result<std::io::Stdin> { Ok(std::io::stdin()) }
#[cfg(not(windows))]
pub fn output() -> std::io::Result<std::io::Stdout> { Ok(std::io::stdout()) }

#[cfg(windows)]
pub struct ParentProcess(std::os::windows::io::OwnedHandle);
#[cfg(windows)]
impl ParentProcess {
    pub fn open(pid: u32) -> std::io::Result<Self> {
        use std::os::windows::io::FromRawHandle;
        use windows::Win32::System::Threading::{OpenProcess, PROCESS_SYNCHRONIZE};
        if pid == std::process::id() || pid == 0 { return Err(std::io::Error::other("Invalid parent process")); }
        let handle = unsafe { OpenProcess(PROCESS_SYNCHRONIZE, false, pid) }.map_err(std::io::Error::other)?;
        Ok(Self(unsafe { std::os::windows::io::OwnedHandle::from_raw_handle(handle.0) }))
    }
    pub fn wait(&self, timeout: Duration) -> std::io::Result<()> {
        use std::os::windows::io::AsRawHandle;
        use windows::Win32::{Foundation::{HANDLE, WAIT_OBJECT_0}, System::Threading::WaitForSingleObject};
        if unsafe { WaitForSingleObject(HANDLE(self.0.as_raw_handle()), timeout.as_millis().min(u32::MAX as u128) as u32) } != WAIT_OBJECT_0 {
            return Err(std::io::Error::other("Previous controller did not exit"));
        }
        Ok(())
    }
}
