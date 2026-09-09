use windows::Win32::{
    Foundation::{HWND, LPARAM, WPARAM},
    Graphics::Dwm::{
        DwmSetWindowAttribute, DWMWA_CAPTION_COLOR, DWMWA_TEXT_COLOR, DWMWA_USE_IMMERSIVE_DARK_MODE,
    },
    UI::WindowsAndMessaging::{
        GetWindowLongPtrW, SendMessageW, SetWindowLongPtrW, SetWindowPos, GWL_EXSTYLE, ICON_SMALL,
        SWP_FRAMECHANGED, SWP_NOACTIVATE, SWP_NOMOVE, SWP_NOSIZE, SWP_NOZORDER, WM_SETICON,
        WS_EX_DLGMODALFRAME,
    },
};

/// Preserve native caption hit testing, snap, system menu and taskbar identity.
/// Only the small caption icon is cleared; the large taskbar icon is retained.
pub(crate) fn style_main_caption(raw: *mut std::ffi::c_void, rgb: u32) {
    let hwnd = HWND(raw);
    let red = (rgb >> 16) & 255;
    let green = (rgb >> 8) & 255;
    let blue = rgb & 255;
    let color: u32 = red | (green << 8) | (blue << 16); // COLORREF is BGR.
    let dark: i32 = i32::from(red * 299 + green * 587 + blue * 114 < 128_000);
    let text: u32 = if dark != 0 { 0x00e8e8e8 } else { 0x00232323 };
    unsafe {
        // Older Windows versions can reject custom caption colors. Keep their
        // native frame usable and use light/dark appearance where supported.
        for (attribute, value) in [
            (DWMWA_USE_IMMERSIVE_DARK_MODE, dark as u32),
            (DWMWA_CAPTION_COLOR, color),
            (DWMWA_TEXT_COLOR, text),
        ] {
            let _ = DwmSetWindowAttribute(hwnd, attribute, std::ptr::from_ref(&value).cast(), 4);
        }
        let style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_DLGMODALFRAME.0 as isize);
        // The previous HICON is owned by the runtime; do not destroy it.
        SendMessageW(
            hwnd,
            WM_SETICON,
            Some(WPARAM(ICON_SMALL as usize)),
            Some(LPARAM(0)),
        );
        let _ = SetWindowPos(
            hwnd,
            None,
            0,
            0,
            0,
            0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE,
        );
    }
}
