//! Paint the legacy notification card with GDI, without a browser or theme controls.
use std::sync::OnceLock;
use windows::{
    core::{w, PCWSTR},
    Win32::{
        Foundation::{COLORREF, HWND, LPARAM, LRESULT, RECT, WPARAM},
        Graphics::Gdi::*,
        UI::WindowsAndMessaging::*,
    },
};

pub const WIDTH: f64 = 350.0;
pub const HEIGHT: f64 = 100.0;
pub const ALPHA: u8 = 242; // round(255 * legacy CSS opacity 0.95)
const BACKGROUND: COLORREF = COLORREF(20 | (21 << 8) | (26 << 16));
const CLASS: PCWSTR = w!("Clipture.NotificationCard.v1");

pub fn set_message(
    parent: HWND,
    message: &str,
    width: i32,
    height: i32,
) -> windows::core::Result<()> {
    static CLASS_REGISTERED: OnceLock<bool> = OnceLock::new();
    let registered = *CLASS_REGISTERED.get_or_init(|| unsafe {
        RegisterClassW(&WNDCLASSW {
            lpfnWndProc: Some(window_proc),
            lpszClassName: CLASS,
            ..Default::default()
        }) != 0
    });
    if !registered {
        return Err(windows::core::Error::from_win32());
    }
    let text: Vec<u16> = message.encode_utf16().chain(Some(0)).collect();
    unsafe {
        let child = match GetWindow(parent, GW_CHILD) {
            Ok(child) => {
                SetWindowTextW(child, PCWSTR(text.as_ptr()))?;
                MoveWindow(child, 0, 0, width, height, true)?;
                child
            }
            Err(_) => CreateWindowExW(
                WINDOW_EX_STYLE::default(),
                CLASS,
                PCWSTR(text.as_ptr()),
                WS_CHILD | WS_VISIBLE,
                0,
                0,
                width,
                height,
                Some(parent),
                None,
                None,
                None,
            )?,
        };
        let _ = InvalidateRect(Some(child), None, false);
        let _ = UpdateWindow(child);
    }
    Ok(())
}

unsafe extern "system" fn window_proc(
    hwnd: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    match message {
        WM_ERASEBKGND => LRESULT(1),
        WM_PAINT => {
            paint(hwnd);
            LRESULT(0)
        }
        _ => DefWindowProcW(hwnd, message, wparam, lparam),
    }
}

unsafe fn paint(hwnd: HWND) {
    let mut paint = PAINTSTRUCT::default();
    let dc = BeginPaint(hwnd, &mut paint);
    let mut rect = RECT::default();
    let _ = GetClientRect(hwnd, &mut rect);
    let mut text = [0u16; 512];
    let length = GetWindowTextW(hwnd, &mut text).max(0) as usize;
    draw_card(dc, rect, &mut text[..length]);
    let _ = EndPaint(hwnd, &paint);
}

unsafe fn draw_card(dc: HDC, rect: RECT, text: &mut [u16]) {
    let scale = (rect.right as f64 / WIDTH).max(0.1);
    let px = |value: f64| (value * scale).round() as i32;
    SetDCBrushColor(dc, BACKGROUND);
    FillRect(dc, &rect, HBRUSH(GetStockObject(DC_BRUSH).0));
    let old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    let old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    SetDCPenColor(dc, COLORREF(0x00aaaaaa));
    // Film icon: 32 px centered in the legacy 64 px thumbnail slot.
    let _ = Rectangle(dc, px(28.0), px(34.0), px(60.0), px(66.0));
    for x in [36.0, 52.0] {
        let _ = MoveToEx(dc, px(x), px(34.0), None);
        let _ = LineTo(dc, px(x), px(66.0));
    }
    for y in [42.0, 50.0, 58.0] {
        for (left, right) in [(28.0, 36.0), (52.0, 60.0)] {
            let _ = MoveToEx(dc, px(left), px(y), None);
            let _ = LineTo(dc, px(right), px(y));
        }
    }
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    let font = CreateFontW(
        -px(16.0),
        0,
        0,
        0,
        700,
        0,
        0,
        0,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        0,
        w!("Segoe UI"),
    );
    let old_font = if !font.is_invalid() {
        Some(SelectObject(dc, HGDIOBJ(font.0)))
    } else {
        None
    };
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COLORREF(0x00ffffff));
    let mut text_rect = RECT {
        left: px(92.0),
        top: 0,
        right: rect.right - px(12.0),
        bottom: rect.bottom,
    };
    DrawTextW(
        dc,
        text,
        &mut text_rect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX,
    );
    if let Some(old_font) = old_font {
        SelectObject(dc, old_font);
        let _ = DeleteObject(HGDIOBJ(font.0));
    }
}

#[cfg(test)]
#[path = "notification_paint_tests.rs"]
mod paint_tests;

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn appearance_matches_legacy_card() {
        assert_eq!((WIDTH, HEIGHT, ALPHA), (350.0, 100.0, 242));
        assert_eq!(BACKGROUND.0, 0x001a1514);
    }

    #[test]
    fn native_card_creates_and_updates_without_a_webview() {
        unsafe {
            let parent = CreateWindowExW(
                WINDOW_EX_STYLE::default(),
                w!("STATIC"),
                w!("notification-test"),
                WS_POPUP,
                0,
                0,
                350,
                100,
                None,
                None,
                None,
                None,
            )
            .unwrap();
            struct OwnedWindow(HWND);
            impl Drop for OwnedWindow {
                fn drop(&mut self) {
                    unsafe {
                        let _ = DestroyWindow(self.0);
                    }
                }
            }
            let _owned = OwnedWindow(parent);
            set_message(parent, "Saving clip...", 350, 100).unwrap();
            let child = GetWindow(parent, GW_CHILD).unwrap();
            set_message(parent, "Clip saved!", 438, 125).unwrap();
            assert_eq!(GetWindow(parent, GW_CHILD).unwrap(), child);
            let mut text = [0; 64];
            let length = GetWindowTextW(child, &mut text) as usize;
            assert_eq!(String::from_utf16_lossy(&text[..length]), "Clip saved!");
            let mut rect = RECT::default();
            GetClientRect(child, &mut rect).unwrap();
            assert_eq!((rect.right, rect.bottom), (438, 125));
        }
    }
}
