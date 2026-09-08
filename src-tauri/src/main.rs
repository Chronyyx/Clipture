#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    if let Some(code) = clipture_lib::migration::installer_command() {
        std::process::exit(code);
    }
    clipture_lib::run();
}
