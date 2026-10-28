//! Where signed releases come from: GitHub over HTTPS. A disposable sandbox
//! test build may be compiled with `CLIPTURE_TEST_RELEASE_ORIGIN` (for example
//! `http://127.0.0.1:8787`) to fetch the same paths from a local mirror
//! (ADR 0020). Signatures are verified either way; release CI refuses the
//! variable, see `scripts/release/validate-production-key.cjs`.

const TEST_ORIGIN: Option<&str> = option_env!("CLIPTURE_TEST_RELEASE_ORIGIN");

fn api() -> String { TEST_ORIGIN.map_or_else(|| "https://api.github.com".to_owned(), str::to_owned) }
fn site() -> String { TEST_ORIGIN.map_or_else(|| "https://github.com".to_owned(), str::to_owned) }

pub fn metadata_url() -> String { format!("{}/repos/Chronyyx/Clipture/releases/latest", api()) }

#[cfg(test)]
thread_local! {
    /// Lets one test serve release assets from its loopback server without
    /// affecting tests on other threads.
    pub static TEST_ASSET_BASE: std::cell::RefCell<Option<String>> = const { std::cell::RefCell::new(None) };
}

/// `<asset base>/v<version>/<file>` names one release asset.
pub fn asset_base() -> String {
    #[cfg(test)]
    if let Some(base) = TEST_ASSET_BASE.with(|base| base.borrow().clone()) {
        return base;
    }
    format!("{}/Chronyyx/Clipture/releases/download", site())
}

/// HTTPS, the unit-test loopback server, or the compiled-in test mirror.
pub fn allows(url: &reqwest::Url) -> bool {
    url.scheme() == "https"
        || (cfg!(test) && url.host_str() == Some("127.0.0.1"))
        || TEST_ORIGIN.is_some_and(|origin| url.as_str().starts_with(&format!("{}/", origin.trim_end_matches('/'))))
}
