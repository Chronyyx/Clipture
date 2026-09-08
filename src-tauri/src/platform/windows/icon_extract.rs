use std::{ffi::c_void, mem::size_of, os::windows::ffi::OsStrExt, path::Path, slice};

use windows::{
    core::PCWSTR,
    Win32::{
        Graphics::Gdi::{
            CreateCompatibleDC, CreateDIBSection, DeleteDC, DeleteObject, SelectObject, BITMAPINFO,
            BITMAPINFOHEADER, BI_RGB, DIB_RGB_COLORS, HGDIOBJ,
        },
        UI::{
            Shell::{SHGetFileInfoW, SHFILEINFOW, SHGFI_ICON, SHGFI_LARGEICON, SHGFI_SMALLICON},
            WindowsAndMessaging::{DestroyIcon, DrawIconEx, DI_NORMAL, HICON},
        },
    },
};

pub fn extract_icon_png(path: &Path, size: u32) -> Result<Option<Vec<u8>>, String> {
    let wide: Vec<u16> = path
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    let mut shell_info = SHFILEINFOW::default();
    let icon_flag = if size <= 20 {
        SHGFI_SMALLICON
    } else {
        SHGFI_LARGEICON
    };
    // SAFETY: the path is NUL-terminated and `shell_info` has the required size.
    let found = unsafe {
        SHGetFileInfoW(
            PCWSTR(wide.as_ptr()),
            Default::default(),
            Some(&mut shell_info),
            size_of::<SHFILEINFOW>() as u32,
            SHGFI_ICON | icon_flag,
        )
    };
    if found == 0 || shell_info.hIcon.0.is_null() {
        return Ok(None);
    }

    let rendered = render_icon(shell_info.hIcon, size);
    // SAFETY: SHGetFileInfoW transferred ownership of this icon handle.
    let _ = unsafe { DestroyIcon(shell_info.hIcon) };
    rendered.and_then(|rgba| encode_png(&rgba, size).map(Some))
}

fn render_icon(icon: HICON, size: u32) -> Result<Vec<u8>, String> {
    // SAFETY: each created GDI object is restored and released before return.
    unsafe {
        let dc = CreateCompatibleDC(None);
        if dc.0.is_null() {
            return Err("CreateCompatibleDC failed while extracting an icon".into());
        }
        let mut info = BITMAPINFO::default();
        info.bmiHeader = BITMAPINFOHEADER {
            biSize: size_of::<BITMAPINFOHEADER>() as u32,
            biWidth: size as i32,
            biHeight: -(size as i32),
            biPlanes: 1,
            biBitCount: 32,
            biCompression: BI_RGB.0,
            biSizeImage: size.saturating_mul(size).saturating_mul(4),
            ..BITMAPINFOHEADER::default()
        };
        let mut bits: *mut c_void = std::ptr::null_mut();
        let bitmap = match CreateDIBSection(None, &info, DIB_RGB_COLORS, &mut bits, None, 0) {
            Ok(bitmap) => bitmap,
            Err(error) => {
                let _ = DeleteDC(dc);
                return Err(format!("CreateDIBSection failed: {error}"));
            }
        };
        if bits.is_null() {
            let _ = DeleteObject(HGDIOBJ(bitmap.0));
            let _ = DeleteDC(dc);
            return Err("CreateDIBSection returned no pixel buffer".into());
        }
        let previous = SelectObject(dc, HGDIOBJ(bitmap.0));
        let draw = DrawIconEx(dc, 0, 0, icon, size as i32, size as i32, 0, None, DI_NORMAL);
        let byte_count = size as usize * size as usize * 4;
        let bgra = slice::from_raw_parts(bits.cast::<u8>(), byte_count);
        let mut rgba = Vec::with_capacity(byte_count);
        for pixel in bgra.chunks_exact(4) {
            rgba.extend_from_slice(&[pixel[2], pixel[1], pixel[0], pixel[3]]);
        }
        let _ = SelectObject(dc, previous);
        let _ = DeleteObject(HGDIOBJ(bitmap.0));
        let _ = DeleteDC(dc);
        draw.map_err(|error| format!("DrawIconEx failed: {error}"))?;

        if rgba.chunks_exact(4).all(|pixel| pixel[3] == 0) {
            for pixel in rgba.chunks_exact_mut(4) {
                pixel[3] = 255;
            }
        }
        Ok(rgba)
    }
}

fn encode_png(rgba: &[u8], size: u32) -> Result<Vec<u8>, String> {
    let mut output = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut output, size, size);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder
            .write_header()
            .map_err(|error| format!("could not encode icon PNG header: {error}"))?;
        writer
            .write_image_data(rgba)
            .map_err(|error| format!("could not encode icon PNG pixels: {error}"))?;
    }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rgba_encoder_produces_a_png() {
        let encoded = encode_png(&[255, 0, 0, 255], 1).unwrap();
        assert_eq!(&encoded[..8], b"\x89PNG\r\n\x1a\n");
    }
}
