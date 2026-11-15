use windows::Win32::{
    Foundation::{HWND, POINT},
    Graphics::{
        Dwm::{DwmSetWindowAttribute, DWMWA_BORDER_COLOR, DWMWA_COLOR_DEFAULT, DWMWA_COLOR_NONE},
        Gdi::{CreatePolygonRgn, SetWindowRgn, WINDING},
    },
};

/// Cuts the window to `points` (window coordinates, physical pixels), or
/// restores the full rectangle when `points` is `None`. Windows owns the
/// region once it is set, so it is not freed here.
pub(crate) fn set_window_region(raw: *mut std::ffi::c_void, points: Option<&[(i32, i32)]>) {
    let hwnd = HWND(raw);
    unsafe {
        match points {
            Some(points) if points.len() >= 3 => {
                let points: Vec<POINT> = points.iter().map(|&(x, y)| POINT { x, y }).collect();
                let region = CreatePolygonRgn(&points, WINDING);
                if !region.is_invalid() {
                    let _ = SetWindowRgn(hwnd, Some(region), true);
                }
            }
            _ => {
                let _ = SetWindowRgn(hwnd, None, true);
            }
        }
    }
}

/// Windows 11 outlines a frameless window's rectangle with a one-pixel
/// border; a shaped window has no rectangle to outline, so it is switched
/// off (`hidden`) or given back. Older Windows ignores the request.
pub(crate) fn hide_window_border(raw: *mut std::ffi::c_void, hidden: bool) {
    let colour: u32 = if hidden { DWMWA_COLOR_NONE } else { DWMWA_COLOR_DEFAULT };
    unsafe {
        let _ = DwmSetWindowAttribute(HWND(raw), DWMWA_BORDER_COLOR, std::ptr::from_ref(&colour).cast(), 4);
    }
}
