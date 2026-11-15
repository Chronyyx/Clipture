//! Window-only presentation. Reuses settings snapshots/events; no new host API.
use crate::contracts::{ClipSettings, UiTheme};
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Mutex,
};
use std::time::Duration;

use tauri::{Listener, WebviewWindow, WindowEvent};

/// The look last applied (background, framed, opaque). Moving to a monitor
/// with another scale makes Windows rebuild the window's surface, which can
/// drop a clear background back to grey, so it is applied again then.
static LAST_LOOK: Mutex<(u32, bool, bool)> = Mutex::new((0x111110, true, true));

/// Counts resizes, so only the last one of a drag restores the window's shape.
static RESIZES: AtomicU64 = AtomicU64::new(0);
/// How long the size must hold still before the shell's cut is put back.
const SHAPE_SETTLE: Duration = Duration::from_millis(160);

pub fn watch(window: &WebviewWindow) {
    apply_background(window, 0x111110, true, true);
    let themed_window = window.clone();
    let subscription = window.listen("settings://changed", move |event| {
        if let Ok(settings) = serde_json::from_str::<ClipSettings>(event.payload()) {
            apply(&themed_window, &settings);
        }
    });
    let closing_window = window.clone();
    window.on_window_event(move |event| match event {
        WindowEvent::Destroyed => closing_window.unlisten(subscription),
        WindowEvent::ScaleFactorChanged { .. } => reapply(&closing_window),
        WindowEvent::Resized(_) => reshape_after_resize(&closing_window),
        _ => {}
    });
}

fn reapply(window: &WebviewWindow) {
    let (background, framed, opaque) = *LAST_LOOK.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    apply_background(window, background, framed, opaque);
}

/// Aero outside a maximized window is cut to its shell's outline with a
/// window region, as Windows XP skins were: nothing transparent is left for
/// Windows to tint on SDR monitors, and clicks outside the shell reach what
/// lies behind it. Every other look, and a maximized window, keeps the full
/// rectangle.
/// While the window is being resized the page repaints a moment after the
/// window changes size, so a cut for the new size would slice through the
/// old picture. The cut is lifted during the drag and put back once the
/// size has held still.
fn reshape_after_resize(window: &WebviewWindow) {
    let (_, _, opaque) = *LAST_LOOK.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    if opaque {
        return;
    }
    let resize = RESIZES.fetch_add(1, Ordering::SeqCst) + 1;
    #[cfg(windows)]
    if let Ok(hwnd) = window.hwnd() {
        crate::platform::windows::set_window_region(hwnd.0, None);
    }
    let settled = window.clone();
    std::thread::spawn(move || {
        std::thread::sleep(SHAPE_SETTLE);
        if RESIZES.load(Ordering::SeqCst) == resize {
            let target = settled.clone();
            let _ = settled.run_on_main_thread(move || update_shape(&target));
        }
    });
}

fn update_shape(window: &WebviewWindow) {
    #[cfg(windows)]
    {
        let (_, _, opaque) = *LAST_LOOK.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let Ok(hwnd) = window.hwnd() else { return };
        let fills = window.is_maximized().unwrap_or(false) || window.is_fullscreen().unwrap_or(false);
        if opaque || fills {
            crate::platform::windows::set_window_region(hwnd.0, None);
            return;
        }
        let (Ok(size), Ok(scale)) = (window.inner_size(), window.scale_factor()) else { return };
        // Cut a pixel inside the drawn edge, so every pixel left in the
        // window is solid shell. Any see-through margin, even a few pixels
        // wide, shows as a grey band on some displays (HDR set-ups).
        let polygon = super::aero_outline::window_polygon(f64::from(size.width), f64::from(size.height), scale, -1.0);
        crate::platform::windows::set_window_region(hwnd.0, Some(&polygon));
    }
    #[cfg(not(windows))]
    let _ = window;
}

pub fn apply(window: &WebviewWindow, settings: &ClipSettings) {
    #[cfg(windows)]
    {
        // Light and Aero draw their own window controls, so they have no
        // system frame.
        let framed = !matches!(settings.ui_theme, UiTheme::Light | UiTheme::Aero);
        let background = match settings.ui_theme {
            UiTheme::Light => 0xf1f1ed,
            UiTheme::Glitten => 0xe5dccb,
            UiTheme::Aero => 0x0a4cc4,
            UiTheme::MaidCafe => 0xffe3ec,
            UiTheme::Halloween => 0x1b1530,
            UiTheme::Custom => parse_color(&settings.custom_main_color).unwrap_or(0x111110),
            _ => 0x111110,
        };
        // Aero is a shaped skin: outside a maximized window only its shell
        // shows, so the window behind the page is fully clear.
        let opaque = !matches!(settings.ui_theme, UiTheme::Aero);
        apply_background(window, background, framed, opaque);
    }
    #[cfg(not(windows))]
    let _ = (window, settings);
}

fn apply_background(window: &WebviewWindow, background: u32, framed: bool, opaque: bool) {
    *LAST_LOOK.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = (background, framed, opaque);
    #[cfg(windows)]
    {
        let window = window.clone();
        let dispatcher = window.clone();
        // Win32 frame mutation stays on the window's owning thread.
        let _ = dispatcher.run_on_main_thread(move || {
            // Windows blends a clear window as premultiplied colour: clear must
            // be transparent black. Any other colour at zero alpha is added to
            // what lies behind, showing as a grey cast on SDR monitors and the
            // colour itself while resizing.
            let colour = if opaque {
                tauri::webview::Color((background >> 16) as u8, (background >> 8) as u8, background as u8, 255)
            } else {
                tauri::webview::Color(0, 0, 0, 0)
            };
            let _ = window.set_background_color(Some(colour));
            if window.is_decorated().ok() != Some(framed) {
                let _ = window.set_decorations(framed);
            }
            // A clear window draws its own shadow round its shape; the system
            // shadow would outline the whole rectangle with a hairline border.
            let _ = window.set_shadow(opaque);
            if let Ok(hwnd) = window.hwnd() {
                crate::platform::windows::hide_window_border(hwnd.0, !opaque);
            }
            update_shape(&window);
            if framed {
                if let Ok(hwnd) = window.hwnd() {
                    crate::platform::windows::style_main_caption(hwnd.0, background);
                }
            }
        });
    }
    #[cfg(not(windows))]
    let _ = (window, background, framed, opaque);
}

#[cfg(windows)]
fn parse_color(value: &str) -> Option<u32> {
    let hex = value.strip_prefix('#')?;
    (hex.len() == 6)
        .then(|| u32::from_str_radix(hex, 16).ok())
        .flatten()
}
