use super::*;

#[test]
fn offscreen_card_has_charcoal_corners_white_text_and_gray_icon() {
    unsafe {
        let dc = CreateCompatibleDC(None);
        assert!(!dc.is_invalid());
        let info = BITMAPINFO {
            bmiHeader: BITMAPINFOHEADER {
                biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
                biWidth: 350,
                biHeight: -100,
                biPlanes: 1,
                biBitCount: 32,
                biCompression: BI_RGB.0,
                ..Default::default()
            },
            ..Default::default()
        };
        let mut bits = std::ptr::null_mut();
        let bitmap = CreateDIBSection(Some(dc), &info, DIB_RGB_COLORS, &mut bits, None, 0).unwrap();
        let old = SelectObject(dc, HGDIOBJ(bitmap.0));
        struct Surface(HDC, HBITMAP, HGDIOBJ);
        impl Drop for Surface {
            fn drop(&mut self) {
                unsafe {
                    SelectObject(self.0, self.2);
                    let _ = DeleteObject(HGDIOBJ(self.1 .0));
                    let _ = DeleteDC(self.0);
                }
            }
        }
        let _surface = Surface(dc, bitmap, old);
        let mut text: Vec<u16> = "Clip saved!".encode_utf16().collect();
        draw_card(
            dc,
            RECT {
                left: 0,
                top: 0,
                right: 350,
                bottom: 100,
            },
            &mut text,
        );
        let _ = GdiFlush();
        for (x, y) in [(0, 0), (349, 0), (0, 99), (349, 99), (12, 12)] {
            assert_eq!(GetPixel(dc, x, y), BACKGROUND);
        }
        assert_eq!(GetPixel(dc, 28, 40), COLORREF(0x00aaaaaa));
        let pixels = std::slice::from_raw_parts(bits.cast::<u8>(), 350 * 100 * 4);
        assert!(
            pixels
                .chunks_exact(4)
                .any(|pixel| pixel[..3] == [255, 255, 255]),
            "white text must be painted"
        );
        // Optional synthetic render artifact, not a desktop screenshot.
        if let Some(path) = std::env::var_os("CLIPTURE_NOTIFICATION_PREVIEW") {
            let file = std::fs::File::create(path).unwrap();
            let mut encoder = png::Encoder::new(file, 350, 100);
            encoder.set_color(png::ColorType::Rgba);
            encoder.set_depth(png::BitDepth::Eight);
            let mut writer = encoder.write_header().unwrap();
            let rgba: Vec<u8> = pixels
                .chunks_exact(4)
                .flat_map(|pixel| [pixel[2], pixel[1], pixel[0], ALPHA])
                .collect();
            writer.write_image_data(&rgba).unwrap();
        }
    }
}
