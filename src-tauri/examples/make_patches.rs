//! Release tool: zstd patches from earlier releases to the new runtime, plus
//! the unsigned `patches-v1.json` that release CI signs (ADR 0020).
//!
//! make_patches <new runtime dir> <version> <previous releases dir> <out dir>
//!
//! `previous releases dir` holds one directory per earlier version, each with
//! that release's runtime assets under their asset names.
fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [runtime, version, previous, out] = args.as_slice() else {
        eprintln!("usage: make_patches <new runtime dir> <version> <previous releases dir> <out dir>");
        std::process::exit(2);
    };
    match clipture_lib::write_runtime_patches(runtime.as_ref(), version, previous.as_ref(), out.as_ref()) {
        Ok(count) => println!("{count} patches written to {out}"),
        Err(error) => {
            eprintln!("make_patches: {error}");
            std::process::exit(1);
        }
    }
}
