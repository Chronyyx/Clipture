//! Keeps a stuck window from staying stuck. A window process whose page
//! never shows, whose page process dies or hangs, or whose own thread stops
//! answering exits with `RESTART`, and the controller opens a fresh one.
//! Without this the window could stay invisible, or blank and see-through,
//! with tray clicks ignored until Clipture was restarted. Recording never
//! depends on the window, so restarting it costs nothing.
use std::{sync::OnceLock, thread, time::Duration};
use tauri::WebviewWindow;

/// Exit code of a window process that gave up on a stuck page.
pub const RESTART: u32 = 0x4352_0001;
/// How long the page may take to show itself. Settings answer within 10 s or
/// the page shows its error screen, so a healthy page is long visible by then.
const REVEAL_PATIENCE: Duration = Duration::from_secs(30);
/// How long the window's thread may take to answer when asked to come forward.
const ANSWER_PATIENCE_MS: u32 = 5_000;
/// The window's handle, taken once on its own thread: asking Tauri for it
/// later waits on that thread, which is the one that may be stuck.
static WINDOW: OnceLock<isize> = OnceLock::new();

pub fn restart(reason: &str) -> ! {
    eprintln!("Clipture window restarting: {reason}");
    std::process::exit(RESTART as i32);
}

/// Watches the page from the moment its window exists.
pub fn watch(window: &WebviewWindow) {
    #[cfg(windows)]
    {
        if let Ok(hwnd) = window.hwnd() {
            let hwnd = hwnd.0 as isize;
            let _ = WINDOW.set(hwnd);
            thread::spawn(move || {
                thread::sleep(REVEAL_PATIENCE);
                if !windows_impl::visible(hwnd) {
                    restart("the page never showed the window");
                }
            });
        }
        let _ = window.with_webview(|webview| windows_impl::watch_processes(&webview));
    }
    #[cfg(not(windows))]
    let _ = (window, REVEAL_PATIENCE);
}

/// Called before bringing the window forward: a window whose thread no
/// longer answers cannot come forward, so a fresh one is started instead.
/// Runs off the window's thread, which may be the one that is stuck.
pub fn ensure_answering() {
    #[cfg(windows)]
    if let Some(&hwnd) = WINDOW.get() {
        if !windows_impl::answers(hwnd, ANSWER_PATIENCE_MS) {
            restart("the window stopped answering");
        }
    }
    #[cfg(not(windows))]
    let _ = ANSWER_PATIENCE_MS;
}

#[cfg(windows)]
mod windows_impl {
    use webview2_com::{
        Microsoft::Web::WebView2::Win32::{
            COREWEBVIEW2_PROCESS_FAILED_KIND, COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED,
            COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED,
            COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE,
        },
        ProcessFailedEventHandler,
    };
    use windows::Win32::{
        Foundation::{HWND, LPARAM, WPARAM},
        UI::WindowsAndMessaging::{IsWindowVisible, SendMessageTimeoutW, SMTO_ABORTIFHUNG, WM_NULL},
    };

    pub fn visible(hwnd: isize) -> bool {
        unsafe { IsWindowVisible(HWND(hwnd as _)) }.as_bool()
    }

    pub fn answers(hwnd: isize, patience_ms: u32) -> bool {
        let sent = unsafe {
            SendMessageTimeoutW(HWND(hwnd as _), WM_NULL, WPARAM(0), LPARAM(0), SMTO_ABORTIFHUNG, patience_ms, None)
        };
        sent.0 != 0
    }

    /// The page's own processes. GPU and helper processes are restarted by
    /// WebView2 itself; a dead or hung page process or browser leaves the
    /// window empty for good.
    pub fn watch_processes(webview: &tauri::webview::PlatformWebview) {
        let Ok(core) = (unsafe { webview.controller().CoreWebView2() }) else { return };
        let handler = ProcessFailedEventHandler::create(Box::new(|_, args| {
            let mut kind = COREWEBVIEW2_PROCESS_FAILED_KIND::default();
            if let Some(args) = args {
                if unsafe { args.ProcessFailedKind(&mut kind) }.is_ok() {
                    if kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED {
                        super::restart("the page process ended");
                    }
                    if kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE {
                        super::restart("the page stopped responding");
                    }
                    if kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED {
                        super::restart("the browser process ended");
                    }
                }
            }
            Ok(())
        }));
        let mut token = 0;
        if let Err(error) = unsafe { core.add_ProcessFailed(&handler, &mut token) } {
            eprintln!("Clipture window cannot watch its page: {error}");
        }
    }
}
