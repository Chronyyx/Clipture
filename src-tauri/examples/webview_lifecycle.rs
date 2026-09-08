//! Raw WebView2 control experiment: no Tauri, Wry, Tao, React or host services.
//! Run only with a new workspace .cache profile, alongside the resource monitor.
#[cfg(windows)]
#[path = "support/process_cycles.rs"]
mod process_cycles;
#[cfg(windows)]
fn main() -> Result<(), Box<dyn std::error::Error>> {
    use std::{
        fs,
        path::PathBuf,
        sync::mpsc,
        time::{Duration, Instant},
    };
    use webview2_com::{Microsoft::Web::WebView2::Win32::*, *};
    use windows::{
        core::*,
        Win32::{Foundation::*, System::Com::*, UI::WindowsAndMessaging::*},
    };

    fn pump(duration: Duration) {
        let until = Instant::now() + duration;
        while Instant::now() < until {
            unsafe {
                let mut message = MSG::default();
                while PeekMessageW(&mut message, None, 0, 0, PM_REMOVE).as_bool() {
                    let _ = TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            std::thread::sleep(Duration::from_millis(5));
        }
    }
    fn sample(profile: &std::path::Path, cycle: usize, phase: &str) -> serde_json::Value {
        if std::env::var("CLIPTURE_RAW_CHILD").as_deref() == Ok("1") {
            if phase == "closed" {
                return serde_json::json!({"ok":true,"workerExiting":true});
            }
            fs::write(profile.join("open-ready"), b"ready").unwrap();
            let deadline = Instant::now() + Duration::from_secs(15);
            while Instant::now() < deadline {
                if let Ok(bytes) = fs::read(profile.join("open-ack.json")) {
                    if let Ok(value) = serde_json::from_slice(&bytes) {
                        return value;
                    }
                }
                pump(Duration::from_millis(20));
            }
            return serde_json::json!({"ok":false,"error":"supervisor did not sample the open worker"});
        }
        let key = format!("{cycle}-{phase}");
        let result = profile.join(format!("resources-{key}.json"));
        fs::write(
            profile.join("resource-request.json"),
            serde_json::json!({"key":key,"phase":phase}).to_string(),
        )
        .unwrap();
        let until = Instant::now() + Duration::from_secs(12);
        while Instant::now() < until {
            if let Ok(text) = fs::read_to_string(&result) {
                if let Ok(value) = serde_json::from_str(text.trim_start_matches('\u{feff}')) {
                    return value;
                }
            }
            pump(Duration::from_millis(50));
        }
        serde_json::json!({"ok":false,"error":"resource monitor timeout"})
    }
    let profile = PathBuf::from(std::env::args().nth(1).ok_or("isolated profile required")?)
        .canonicalize()?;
    let cache = std::env::current_dir()?.join(".cache").canonicalize()?;
    if !profile.starts_with(&cache) || profile == cache {
        return Err("profile must be a child of workspace .cache".into());
    }
    let cycles: usize = std::env::args().nth(2).unwrap_or("10".into()).parse()?;
    if !(1..=100).contains(&cycles) {
        return Err("cycle count must be 1..100".into());
    }
    let per_cycle_thread = std::env::var("CLIPTURE_RAW_THREAD_PER_CYCLE").as_deref() == Ok("1");
    if std::env::var("CLIPTURE_RAW_PROCESS_PER_CYCLE").as_deref() == Ok("1") {
        return process_cycles::run(&profile, cycles, sample);
    }
    let per_cycle_com =
        per_cycle_thread || std::env::var("CLIPTURE_RAW_COM_PER_CYCLE").as_deref() == Ok("1");
    if !per_cycle_com {
        unsafe {
            CoInitializeEx(None, COINIT_APARTMENTTHREADED).ok()?;
        }
    }
    let mut results = Vec::new();
    for cycle in 0..cycles {
        let run_cycle = || -> std::result::Result<serde_json::Value, Box<dyn std::error::Error>> {
            if per_cycle_com {
                unsafe {
                    CoInitializeEx(None, COINIT_APARTMENTTHREADED).ok()?;
                }
            }
            let parent = unsafe {
                CreateWindowExW(
                    Default::default(),
                    w!("STATIC"),
                    w!("Clipture raw WebView2 test"),
                    WS_OVERLAPPEDWINDOW,
                    0,
                    0,
                    320,
                    200,
                    None,
                    None,
                    None,
                    None,
                )?
            };
            let (tx, rx) = mpsc::channel();
            let directory = profile.join("webview2").to_string_lossy().to_string();
            CreateCoreWebView2EnvironmentCompletedHandler::wait_for_async_operation(
                Box::new(move |handler| unsafe {
                    let directory = CoTaskMemPWSTR::from(directory.as_str());
                    CreateCoreWebView2EnvironmentWithOptions(
                        PCWSTR::null(),
                        *directory.as_ref().as_pcwstr(),
                        None,
                        &handler,
                    )
                    .map_err(webview2_com::Error::WindowsError)
                }),
                Box::new(move |error, environment| {
                    error?;
                    tx.send(environment).unwrap();
                    Ok(())
                }),
            )?;
            let environment = rx.recv()?.ok_or("missing WebView2 environment")?;
            let creating = environment.clone();
            let (tx, rx) = mpsc::channel();
            CreateCoreWebView2ControllerCompletedHandler::wait_for_async_operation(
                Box::new(move |handler| unsafe {
                    creating
                        .CreateCoreWebView2Controller(parent, &handler)
                        .map_err(webview2_com::Error::WindowsError)
                }),
                Box::new(move |error, controller| {
                    error?;
                    tx.send(controller).unwrap();
                    Ok(())
                }),
            )?;
            let controller = rx.recv()?.ok_or("missing WebView2 controller")?;
            let webview = unsafe { controller.CoreWebView2()? };
            unsafe {
                controller.SetBounds(RECT {
                    left: 0,
                    top: 0,
                    right: 320,
                    bottom: 200,
                })?;
                controller.SetIsVisible(true)?;
                webview.NavigateToString(w!(
                    "<!doctype html><title>Lifecycle control</title>Raw WebView2"
                ))?;
            }
            pump(Duration::from_secs(1));
            let open = sample(&profile, cycle, "open");
            unsafe {
                controller.Close()?;
            }
            drop(webview);
            drop(controller);
            drop(environment);
            unsafe {
                DestroyWindow(parent)?;
            }
            if per_cycle_com {
                unsafe {
                    CoUninitialize();
                }
            }
            pump(Duration::from_secs(2));
            let closed = sample(&profile, cycle, "closed");
            Ok(serde_json::json!({"cycle":cycle,"open":open,"closed":closed}))
        };
        let result = if per_cycle_thread {
            std::thread::scope(|scope| {
                scope
                    .spawn(|| run_cycle().map_err(|error| error.to_string()))
                    .join()
                    .map_err(|_| "WebView test thread panicked".to_string())?
            })?
        } else {
            run_cycle()?
        };
        results.push(result);
        // Keep partial evidence if a later native call or monitor fails.
        fs::write(
            profile.join("raw-webview-result.json"),
            serde_json::to_vec_pretty(&results)?,
        )?;
    }
    let settle_seconds = std::env::var("CLIPTURE_RAW_SETTLE_SECONDS")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or(0)
        .min(900);
    if settle_seconds > 0 {
        // Observe delayed cleanup without forcing DLL or COM-runtime unload.
        pump(Duration::from_secs(settle_seconds));
        let settled = sample(&profile, cycles, "closed");
        fs::write(
            profile.join("raw-webview-settled.json"),
            serde_json::to_vec_pretty(&settled)?,
        )?;
    }
    if !per_cycle_com {
        unsafe {
            CoUninitialize();
        }
    }
    fs::write(
        profile.join("raw-webview-result.json"),
        serde_json::to_vec_pretty(&results)?,
    )?;
    Ok(())
}

#[cfg(not(windows))]
fn main() {
    panic!("Windows-only integration experiment");
}
