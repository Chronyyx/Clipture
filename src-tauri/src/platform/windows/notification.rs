use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};

use tauri::{AppHandle, Manager, PhysicalPosition, WindowBuilder};
use windows::Win32::{
    Foundation::{COLORREF, HWND},
    Graphics::Dwm::{DwmSetWindowAttribute, DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_DONOTROUND},
    UI::WindowsAndMessaging::{
        GetWindowLongPtrW, SetLayeredWindowAttributes, SetWindowLongPtrW, GWL_EXSTYLE, LWA_ALPHA,
        WS_EX_LAYERED,
    },
};

use crate::{
    error::{AppError, AppResult},
    notifications::{NotificationPlacement, NotificationRequest, NotificationSink},
};

const LABEL: &str = "native-save-feedback";

/// A short-lived, custom-painted Win32 card. No WebView, browser
/// runtime, event subscription, or renderer state participates in save feedback.
pub struct NativeNotificationSink {
    app: AppHandle,
    generation: Arc<AtomicU64>,
}

impl NativeNotificationSink {
    pub fn new(app: AppHandle) -> Self {
        Self {
            app,
            generation: Arc::new(AtomicU64::new(0)),
        }
    }

    #[cfg(debug_assertions)]
    pub(crate) fn verify_window(app: &AppHandle) -> AppResult<()> {
        use windows::Win32::Graphics::Dwm::DwmGetWindowAttribute;
        use windows::Win32::UI::WindowsAndMessaging::{
            GetLayeredWindowAttributes, WS_EX_TRANSPARENT,
        };
        let window = app
            .get_window(LABEL)
            .ok_or_else(|| AppError::Integration("notification window missing".into()))?;
        let size = window
            .inner_size()?
            .to_logical::<f64>(window.scale_factor()?);
        let hwnd = HWND(window.hwnd()?.0);
        let mut alpha = 0;
        unsafe {
            GetLayeredWindowAttributes(hwnd, None, Some(&mut alpha), None)
                .map_err(|error| AppError::Integration(error.to_string()))?;
            let style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
            if alpha != super::notification_paint::ALPHA
                || style & WS_EX_TRANSPARENT.0 == 0
                || (size.width - super::notification_paint::WIDTH).abs() > 1.0
                || (size.height - super::notification_paint::HEIGHT).abs() > 1.0
            {
                return Err(AppError::Integration(
                    "notification opacity, click-through or size regression".into(),
                ));
            }
            let mut corner = 0u32;
            if DwmGetWindowAttribute(
                hwnd,
                DWMWA_WINDOW_CORNER_PREFERENCE,
                std::ptr::from_mut(&mut corner).cast(),
                4,
            )
            .is_ok()
                && corner != DWMWCP_DONOTROUND.0 as u32
            {
                return Err(AppError::Integration(
                    "notification has rounded corners".into(),
                ));
            }
        }
        Ok(())
    }
}

impl NotificationSink for NativeNotificationSink {
    fn show(&self, request: &NotificationRequest) -> AppResult<()> {
        let generation = self.generation.fetch_add(1, Ordering::AcqRel) + 1;
        let current = self.generation.clone();
        let app = self.app.clone();
        let request = request.clone();
        self.app.run_on_main_thread(move || {
            if current.load(Ordering::Acquire) != generation {
                return;
            }
            if let Err(error) = show_window(&app, &request) {
                tracing::warn!(%error, "could not display native save feedback");
                return;
            }
            tauri::async_runtime::spawn(async move {
                tokio::time::sleep(request.visible_for).await;
                let handle = app.clone();
                let _ = app.run_on_main_thread(move || {
                    if current.load(Ordering::Acquire) == generation {
                        if let Some(window) = handle.get_window(LABEL) {
                            let _ = window.destroy();
                        }
                    }
                });
            });
        })?;
        Ok(())
    }

    fn hide(&self) -> AppResult<()> {
        self.generation.fetch_add(1, Ordering::AcqRel);
        let app = self.app.clone();
        self.app.run_on_main_thread(move || {
            if let Some(window) = app.get_window(LABEL) {
                let _ = window.destroy();
            }
        })?;
        Ok(())
    }
}

fn show_window(app: &AppHandle, request: &NotificationRequest) -> AppResult<()> {
    let window = match app.get_window(LABEL) {
        Some(window) => window,
        None => WindowBuilder::new(app, LABEL)
            .title("Clipture")
            .inner_size(
                super::notification_paint::WIDTH,
                super::notification_paint::HEIGHT,
            )
            .decorations(false)
            .shadow(false)
            .resizable(false)
            .always_on_top(true)
            .skip_taskbar(true)
            .focused(false)
            .focusable(false)
            .content_protected(true)
            .visible(false)
            .build()?,
    };
    let result = (|| -> AppResult<()> {
        if let Some(monitor) = window.primary_monitor()? {
            let area = monitor.work_area();
            let size = window.outer_size()?;
            let margin = (30.0 * monitor.scale_factor()).round() as i32;
            let (x, y) = position(
                request.placement,
                area.position.x,
                area.position.y,
                area.size.width as i32,
                area.size.height as i32,
                size.width as i32,
                size.height as i32,
                margin,
            );
            window.set_position(PhysicalPosition::new(x, y))?;
        }
        let size = window.inner_size()?;
        let parent = window.hwnd()?;
        unsafe {
            let hwnd = HWND(parent.0);
            let style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED.0 as isize);
            SetLayeredWindowAttributes(
                hwnd,
                COLORREF(0),
                super::notification_paint::ALPHA,
                LWA_ALPHA,
            )
            .map_err(|error| AppError::Integration(error.to_string()))?;
            // Windows 10 may not implement this Windows 11 attribute. The
            // borderless, shadowless card still works there without it.
            let corner = DWMWCP_DONOTROUND;
            let _ = DwmSetWindowAttribute(
                hwnd,
                DWMWA_WINDOW_CORNER_PREFERENCE,
                std::ptr::from_ref(&corner).cast(),
                std::mem::size_of_val(&corner) as u32,
            );
        }
        super::notification_paint::set_message(
            HWND(parent.0),
            &request.message,
            size.width as i32,
            size.height as i32,
        )
        .map_err(|error| AppError::Integration(error.to_string()))?;
        window.set_ignore_cursor_events(true)?;
        window.show()?;
        Ok(())
    })();
    if result.is_err() {
        let _ = window.destroy();
    }
    result
}

#[allow(clippy::too_many_arguments)]
fn position(
    placement: NotificationPlacement,
    left: i32,
    top: i32,
    width: i32,
    height: i32,
    window_width: i32,
    window_height: i32,
    margin: i32,
) -> (i32, i32) {
    let x = match placement {
        NotificationPlacement::TopLeft | NotificationPlacement::BottomLeft => left,
        NotificationPlacement::TopCenter => left + (width - window_width) / 2,
        _ => left + width - window_width,
    };
    let y = match placement {
        NotificationPlacement::BottomLeft | NotificationPlacement::BottomRight => {
            top + height - window_height - margin
        }
        _ => top + margin,
    };
    (x.max(left), y.max(top))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn left_and_right_cards_touch_the_edge_with_only_vertical_padding() {
        for (placement, expected) in [
            (NotificationPlacement::TopLeft, (-1920, 30)),
            (NotificationPlacement::TopRight, (-350, 30)),
            (NotificationPlacement::BottomLeft, (-1920, 910)),
            (NotificationPlacement::BottomRight, (-350, 910)),
            (NotificationPlacement::TopCenter, (-1135, 30)),
        ] {
            assert_eq!(
                position(placement, -1920, 0, 1920, 1040, 350, 100, 30),
                expected
            );
        }
        assert_eq!(
            position(
                NotificationPlacement::BottomRight,
                0,
                -1080,
                2400,
                1300,
                438,
                125,
                38
            ),
            (1962, 57)
        );
    }
    #[test]
    fn positions_respect_negative_monitor_coordinates_and_work_area() {
        assert_eq!(
            position(
                NotificationPlacement::BottomRight,
                -1920,
                0,
                1920,
                1040,
                300,
                76,
                20
            ),
            (-300, 944)
        );
        assert_eq!(
            position(
                NotificationPlacement::TopCenter,
                -1920,
                0,
                1920,
                1040,
                300,
                76,
                20
            ),
            (-1110, 20)
        );
    }
}
