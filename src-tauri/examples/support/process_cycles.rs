//! Experimental process-lifetime boundary, not the production application.
use std::os::windows::process::CommandExt;
use std::{
    fs,
    path::Path,
    process::{Child, Command},
    time::{Duration, Instant},
};

struct OwnedChild(Child);
impl Drop for OwnedChild {
    fn drop(&mut self) {
        if self.0.try_wait().ok().flatten().is_none() {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
}

pub fn run(
    profile: &Path,
    cycles: usize,
    sample: impl Fn(&Path, usize, &str) -> serde_json::Value,
) -> Result<(), Box<dyn std::error::Error>> {
    let executable = std::env::current_exe()?;
    for cycle in 0..cycles {
        let child_profile = profile.join(format!("worker-{cycle}"));
        fs::create_dir(&child_profile)?;
        let mut child = OwnedChild(
            Command::new(&executable)
                .arg(&child_profile)
                .arg("1")
                .env("CLIPTURE_RAW_PROCESS_PER_CYCLE", "0")
                .env("CLIPTURE_RAW_CHILD", "1")
                .env("CLIPTURE_RAW_SETTLE_SECONDS", "0")
                .env_remove("CLIPTURE_RAW_THREAD_PER_CYCLE")
                .env_remove("CLIPTURE_RAW_COM_PER_CYCLE")
                .creation_flags(0x0800_0000)
                .spawn()?,
        );
        let deadline = Instant::now() + Duration::from_secs(50);
        let mut open = None;
        let status = loop {
            if open.is_none() && child_profile.join("open-ready").exists() {
                let measured = sample(profile, cycle, "open");
                fs::write(
                    child_profile.join("open-ack.json"),
                    serde_json::to_vec(&measured)?,
                )?;
                open = Some(measured);
            }
            if let Some(status) = child.0.try_wait()? {
                break status;
            }
            if Instant::now() >= deadline {
                return Err("disposable WebView worker timed out".into());
            }
            std::thread::sleep(Duration::from_millis(20));
        };
        drop(child); // Release the supervisor's process handle before measuring.
        let closed = sample(profile, cycle, "closed");
        let row = serde_json::json!({"cycle":cycle,"open":open,"closed":closed,"workerExitCode":status.code()});
        fs::write(
            child_profile.join("cycle-result.json"),
            serde_json::to_vec_pretty(&row)?,
        )?;
        if !status.success() || row["open"]["ok"] != true {
            return Err("disposable WebView worker failed before its complete lifecycle".into());
        }
        if (cycle + 1) % 10 == 0 {
            println!("Disposable WebView processes: {}/{cycles}", cycle + 1);
        }
    }
    // Build the aggregate only after the final resource sample. Keeping every
    // process-tree snapshot in RAM would measure the test report's growth.
    let rows: Result<Vec<serde_json::Value>, Box<dyn std::error::Error>> = (0..cycles)
        .map(|cycle| {
            let bytes = fs::read(profile.join(format!("worker-{cycle}/cycle-result.json")))?;
            Ok(serde_json::from_slice(&bytes)?)
        })
        .collect();
    fs::write(
        profile.join("raw-webview-result.json"),
        serde_json::to_vec_pretty(&rows?)?,
    )?;
    Ok(())
}
