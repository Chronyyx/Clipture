//! Own a private child tree without enumerating PIDs or changing browser security.
use std::{
    os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle},
    process::Child,
};
use windows::Win32::{
    Foundation::HANDLE,
    System::JobObjects::{
        AssignProcessToJobObject, CreateJobObjectW, JobObjectExtendedLimitInformation,
        SetInformationJobObject, JOBOBJECT_EXTENDED_LIMIT_INFORMATION,
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
    },
};

pub struct ChildJob(OwnedHandle);
impl ChildJob {
    /// Call while the child is blocked on its private bootstrap, before it can
    /// spawn descendants. The unnamed, non-inheritable job stays in the parent.
    /// Nested jobs retain WebView2's own sandbox jobs on supported Windows.
    pub fn attach(child: &Child) -> Result<Self, windows::core::Error> {
        unsafe {
            let handle = CreateJobObjectW(None, None)?;
            let job = Self(OwnedHandle::from_raw_handle(handle.0));
            let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(
                HANDLE(job.0.as_raw_handle()),
                JobObjectExtendedLimitInformation,
                (&limits as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
                std::mem::size_of_val(&limits) as u32,
            )?;
            AssignProcessToJobObject(HANDLE(job.0.as_raw_handle()), HANDLE(child.as_raw_handle()))?;
            Ok(job)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        os::windows::process::CommandExt,
        process::{Command, Stdio},
        time::{Duration, Instant},
    };
    struct OwnedChild(Child);
    impl Drop for OwnedChild {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
    fn waiting_child() -> OwnedChild {
        OwnedChild(
            Command::new("cmd.exe")
                .args(["/D", "/Q"])
                .stdin(Stdio::piped())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .creation_flags(0x0800_0000)
                .spawn()
                .unwrap(),
        )
    }
    #[test]
    fn closing_private_job_reaps_only_its_owned_child() {
        let mut owned = waiting_child();
        let mut sibling = waiting_child();
        let job = ChildJob::attach(&owned.0).unwrap();
        drop(job);
        let deadline = Instant::now() + Duration::from_secs(2);
        while owned.0.try_wait().unwrap().is_none() && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(owned.0.try_wait().unwrap().is_some());
        assert!(sibling.0.try_wait().unwrap().is_none());
    }
}
