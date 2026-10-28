fn main() {
    // Match the Tauri CLI for direct Cargo builds and standalone diagnostics too.
    // Windows supplies UCRT; do not require a separate VC runtime installation.
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        std::env::set_var("STATIC_VCRUNTIME", "true");
    }
    println!("cargo:rerun-if-env-changed=CLIPTURE_NATIVE_ID");
    println!("cargo:rerun-if-env-changed=CLIPTURE_TEST_RELEASE_ORIGIN");
    if let Ok(origin) = std::env::var("CLIPTURE_TEST_RELEASE_ORIGIN") {
        println!("cargo:warning=Sandbox test build: updates come from {origin}, not GitHub");
    }
    tauri_build::build();
}
