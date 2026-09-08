fn main() {
    // Match the Tauri CLI for direct Cargo builds and standalone diagnostics too.
    // Windows supplies UCRT; do not require a separate VC runtime installation.
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        std::env::set_var("STATIC_VCRUNTIME", "true");
    }
    tauri_build::build();
}
