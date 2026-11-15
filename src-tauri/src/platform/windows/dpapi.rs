//! DPAPI for the current user: data protected here decrypts only for the
//! same Windows account (ADR 0035, the telemetry device key).
use windows::Win32::{
    Foundation::{LocalFree, HLOCAL},
    Security::Cryptography::{CryptProtectData, CryptUnprotectData, CRYPTPROTECT_UI_FORBIDDEN, CRYPT_INTEGER_BLOB},
};

fn blob(bytes: &[u8]) -> CRYPT_INTEGER_BLOB {
    CRYPT_INTEGER_BLOB { cbData: bytes.len() as u32, pbData: bytes.as_ptr() as *mut u8 }
}

fn take(output: CRYPT_INTEGER_BLOB) -> Vec<u8> {
    // SAFETY: on success DPAPI returns a LocalAlloc'd buffer of cbData bytes,
    // which is copied and then freed exactly once.
    unsafe {
        let bytes = std::slice::from_raw_parts(output.pbData, output.cbData as usize).to_vec();
        let _ = LocalFree(Some(HLOCAL(output.pbData.cast())));
        bytes
    }
}

pub fn protect(plain: &[u8]) -> std::io::Result<Vec<u8>> {
    let mut output = CRYPT_INTEGER_BLOB::default();
    // SAFETY: the input blob borrows `plain` for the duration of the call.
    unsafe { CryptProtectData(&blob(plain), None, None, None, None, CRYPTPROTECT_UI_FORBIDDEN, &mut output) }.map_err(std::io::Error::other)?;
    Ok(take(output))
}

pub fn unprotect(protected: &[u8]) -> std::io::Result<Vec<u8>> {
    let mut output = CRYPT_INTEGER_BLOB::default();
    // SAFETY: as above.
    unsafe { CryptUnprotectData(&blob(protected), None, None, None, None, CRYPTPROTECT_UI_FORBIDDEN, &mut output) }.map_err(std::io::Error::other)?;
    Ok(take(output))
}

#[cfg(test)]
mod tests {
    #[test]
    fn round_trips_and_hides_the_plain_bytes() {
        let secret = [0x5a_u8; 32];
        let protected = super::protect(&secret).unwrap();
        assert!(!protected.windows(32).any(|window| window == secret));
        assert_eq!(super::unprotect(&protected).unwrap(), secret);
        assert!(super::unprotect(&protected[..protected.len() - 1]).is_err());
    }
}
