//! Which processes are using the GPU's video encode, video decode and 3D
//! engines right now, from Windows' "GPU Engine" performance counters (the
//! ones Task Manager shows per process). Any GPU vendor, no administrator
//! rights. Telemetry calls this only after a burst of dropped frames, never
//! on a timer: one call costs a counter enumeration and `window` of waiting.
use std::{collections::HashMap, time::Duration};

use windows::{
    core::{w, PCWSTR},
    Win32::{
        Foundation::CloseHandle,
        System::{
            Diagnostics::ToolHelp::{CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS},
            Performance::{
                PdhAddEnglishCounterW, PdhCloseQuery, PdhCollectQueryData, PdhGetFormattedCounterArrayW, PdhOpenQueryW,
                PDH_FMT_COUNTERVALUE_ITEM_W, PDH_FMT_DOUBLE, PDH_HCOUNTER, PDH_HQUERY, PDH_MORE_DATA,
            },
        },
    },
};

/// One process's share of each engine type over the sampling window, in
/// percent of its busiest engine of that type.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct GpuEngineUser {
    pub pid: u32,
    pub parent_pid: u32,
    /// Executable file name as Windows reports it, e.g. `Discord.exe`.
    pub name: String,
    pub encode: f64,
    pub decode: f64,
    pub three_d: f64,
}

struct Query(PDH_HQUERY);

impl Drop for Query {
    fn drop(&mut self) {
        unsafe {
            PdhCloseQuery(self.0);
        }
    }
}

/// Samples the counters twice, `window` apart, and returns every process
/// with any encode, decode or 3D use in between.
pub fn sample(window: Duration) -> Result<Vec<GpuEngineUser>, String> {
    let mut query = PDH_HQUERY::default();
    let status = unsafe { PdhOpenQueryW(PCWSTR::null(), 0, &mut query) };
    if status != 0 {
        return Err(format!("PdhOpenQuery failed: {status:#x}"));
    }
    let query = Query(query);
    let mut counter = PDH_HCOUNTER::default();
    let status = unsafe { PdhAddEnglishCounterW(query.0, w!("\\GPU Engine(*)\\Utilization Percentage"), 0, &mut counter) };
    if status != 0 {
        return Err(format!("GPU Engine counters unavailable: {status:#x}"));
    }
    // A rate counter needs two collections; the first only sets the base.
    unsafe {
        PdhCollectQueryData(query.0);
    }
    std::thread::sleep(window);
    let status = unsafe { PdhCollectQueryData(query.0) };
    if status != 0 {
        return Err(format!("PdhCollectQueryData failed: {status:#x}"));
    }

    let (mut size, mut count) = (0u32, 0u32);
    let status = unsafe { PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &mut size, &mut count, None) };
    if status != PDH_MORE_DATA {
        return Ok(Vec::new());
    }
    // The buffer holds the items followed by their name strings.
    let items = (size as usize).div_ceil(std::mem::size_of::<PDH_FMT_COUNTERVALUE_ITEM_W>());
    let mut buffer = vec![PDH_FMT_COUNTERVALUE_ITEM_W::default(); items.max(1)];
    let status = unsafe { PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &mut size, &mut count, Some(buffer.as_mut_ptr())) };
    if status != 0 {
        return Err(format!("PdhGetFormattedCounterArray failed: {status:#x}"));
    }

    let mut by_pid: HashMap<u32, GpuEngineUser> = HashMap::new();
    for item in buffer.iter().take(count as usize) {
        if item.FmtValue.CStatus != 0 {
            continue;
        }
        let value = unsafe { item.FmtValue.Anonymous.doubleValue };
        if value <= 0.0 {
            continue;
        }
        let name = unsafe { item.szName.to_string() }.unwrap_or_default();
        let Some((pid, engine)) = parse_instance(&name) else { continue };
        let user = by_pid.entry(pid).or_insert_with(|| GpuEngineUser { pid, ..Default::default() });
        // A GPU has several engines of a type (3D and compute queues, more
        // than one video engine); the busiest one is how busy that type is.
        // Summing them reads 3D and compute as one engine above 100 %.
        let slot = match engine {
            "videoencode" => &mut user.encode,
            "videodecode" => &mut user.decode,
            "3d" => &mut user.three_d,
            _ => continue,
        };
        *slot = slot.max(value);
    }
    drop(query);
    if by_pid.is_empty() {
        return Ok(Vec::new());
    }
    for (pid, (parent, name)) in processes() {
        if let Some(user) = by_pid.get_mut(&pid) {
            user.parent_pid = parent;
            user.name = name;
        }
    }
    Ok(by_pid.into_values().filter(|user| user.encode > 0.0 || user.decode > 0.0 || user.three_d > 0.0).collect())
}

/// `pid_1234_luid_0x0_0xD1B6_phys_0_eng_5_engtype_VideoEncode` →
/// (1234, "videoencode").
fn parse_instance(name: &str) -> Option<(u32, &'static str)> {
    let lower = name.to_ascii_lowercase();
    let pid = lower.strip_prefix("pid_")?.split('_').next()?.parse().ok()?;
    let engine = lower.rsplit("engtype_").next()?;
    let engine = match engine {
        "videoencode" => "videoencode",
        "videodecode" | "videoprocessing" => "videodecode",
        "3d" | "graphics_1" | "compute_0" | "compute_1" => "3d",
        _ => return None,
    };
    Some((pid, engine))
}

/// Every running process: pid → (parent pid, executable name).
pub fn processes() -> HashMap<u32, (u32, String)> {
    let mut out = HashMap::new();
    let Ok(snapshot) = (unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) }) else { return out };
    let mut entry = PROCESSENTRY32W { dwSize: std::mem::size_of::<PROCESSENTRY32W>() as u32, ..Default::default() };
    let mut more = unsafe { Process32FirstW(snapshot, &mut entry) }.is_ok();
    while more {
        let length = entry.szExeFile.iter().position(|c| *c == 0).unwrap_or(entry.szExeFile.len());
        out.insert(entry.th32ProcessID, (entry.th32ParentProcessID, String::from_utf16_lossy(&entry.szExeFile[..length])));
        more = unsafe { Process32NextW(snapshot, &mut entry) }.is_ok();
    }
    unsafe {
        let _ = CloseHandle(snapshot);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn counter_instances_name_the_process_and_engine() {
        assert_eq!(parse_instance("pid_1234_luid_0x00000000_0x0000D1B6_phys_0_eng_5_engtype_VideoEncode"), Some((1234, "videoencode")));
        assert_eq!(parse_instance("pid_88_luid_0x00000000_0x0000D1B6_phys_0_eng_0_engtype_3D"), Some((88, "3d")));
        assert_eq!(parse_instance("pid_88_luid_0x00000000_0x0000D1B6_phys_0_eng_2_engtype_Copy"), None);
        assert_eq!(parse_instance("garbage"), None);
    }

    fn thread_cpu_ms() -> f64 {
        use windows::Win32::{Foundation::FILETIME, System::Threading::{GetCurrentThread, GetThreadTimes}};
        let (mut a, mut b, mut kernel, mut user) = (FILETIME::default(), FILETIME::default(), FILETIME::default(), FILETIME::default());
        unsafe { GetThreadTimes(GetCurrentThread(), &mut a, &mut b, &mut kernel, &mut user).unwrap() };
        let ticks = |t: FILETIME| ((t.dwHighDateTime as u64) << 32 | t.dwLowDateTime as u64) as f64;
        (ticks(kernel) + ticks(user)) / 10_000.0
    }

    /// What one probe costs in CPU on this machine (run with --nocapture).
    /// Needs a GPU, so not in CI: `cargo test --lib gpu_engine_users -- --ignored`.
    #[test]
    #[ignore]
    fn probe_cpu_cost() {
        for round in 0..3 {
            let (wall, cpu) = (std::time::Instant::now(), thread_cpu_ms());
            let users = sample(Duration::from_millis(250)).unwrap();
            eprintln!("round {round}: wall {:?}, cpu {:.0} ms, {} users", wall.elapsed(), thread_cpu_ms() - cpu, users.len());
        }
        let (wall, cpu) = (std::time::Instant::now(), thread_cpu_ms());
        let count = processes().len();
        eprintln!("process snapshot: wall {:?}, cpu {:.1} ms, {count} processes", wall.elapsed(), thread_cpu_ms() - cpu);
    }

    /// The real probe on this machine: it must answer, and quickly.
    /// Needs a GPU, so not in CI: `cargo test --lib gpu_engine_users -- --ignored`.
    #[test]
    #[ignore]
    fn the_probe_runs_on_this_machine() {
        let started = std::time::Instant::now();
        let users = sample(Duration::from_millis(250)).expect("GPU Engine counters");
        let elapsed = started.elapsed();
        eprintln!("probe: {} users in {elapsed:?}", users.len());
        assert!(elapsed < Duration::from_secs(3), "{elapsed:?}");
    }
}
