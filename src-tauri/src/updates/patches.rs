//! Tiny binary patches between releases (ADR 0020).
//!
//! A release may list zstd `--patch-from` patches from recent releases in
//! `patches-v1.json`, signed with the updater key. A patch is only a shortcut:
//! the bytes it produces must still match the signed runtime manifest, and
//! any failure falls back to the normal differential download.
use super::{
    component_download,
    manifest::{invalid, valid_hash, verify_file, Manifest, RuntimeFile, FILE_LIMIT, PATHS},
    release_source,
    service::{UpdateError, UpdateGate},
    stream_download::{verify_before_install, wait_until_allowed},
};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{fs, io::{Read, Write}, path::Path};
use zstd::zstd_safe::{CCtx, CParameter, DCtx, DParameter};

pub const LIST: &str = "patches-v1.json";
/// 128 MiB reach into the old file: enough for every runtime file today.
const WINDOW_LOG: u32 = 27;
const PATCH_LIMIT: u64 = 64 * 1024 * 1024;
const LIST_LIMIT: usize = 256 * 1024;
const LEVEL: i32 = 19;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Patch {
    pub path: String,
    /// SHA-256 of the installed file this patch starts from.
    pub from_sha256: String,
    /// SHA-256 and size of the patch itself.
    pub sha256: String,
    pub size: u64,
    pub url: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct PatchList {
    pub schema_version: u32,
    pub version: String,
    pub patches: Vec<Patch>,
}

/// `clipture.exe.from-<16 hex>.zst`: one asset per file and starting point.
pub fn file_name(path: &str, from_sha256: &str) -> String {
    format!("{}.from-{}.zst", path.rsplit('/').next().unwrap_or(path), &from_sha256[..16.min(from_sha256.len())])
}

fn asset_url(version: &str, name: &str) -> String { format!("{}/v{version}/{name}", release_source::asset_base()) }

impl PatchList {
    fn validate(&self, manifest: &Manifest) -> Result<(), UpdateError> {
        if self.schema_version != 1 || self.version != manifest.version || self.patches.len() > 64 {
            return Err(invalid("Unsupported patch list"));
        }
        for patch in &self.patches {
            let target = manifest.files.iter().find(|file| file.path == patch.path);
            let expected_url = asset_url(&self.version, &file_name(&patch.path, &patch.from_sha256));
            let local_test = cfg!(test) && patch.url.starts_with("http://127.0.0.1:");
            if !PATHS.contains(&patch.path.as_str()) || !valid_hash(&patch.from_sha256) || !valid_hash(&patch.sha256)
                || target.is_none_or(|target| target.sha256 == patch.from_sha256 || patch.size >= target.size)
                || patch.size == 0 || patch.size > PATCH_LIMIT
                || (patch.url != expected_url && !local_test)
            {
                return Err(invalid("Invalid patch entry"));
            }
        }
        Ok(())
    }

    /// The patch that turns the installed file at `base` into `target`.
    pub fn find(&self, target: &RuntimeFile, base: &Path) -> Option<&Patch> {
        let candidates: Vec<_> = self.patches.iter().filter(|patch| patch.path == target.path).collect();
        if candidates.is_empty() { return None; }
        let installed = sha256_file(base).ok()?;
        candidates.into_iter().find(|patch| patch.from_sha256 == installed)
    }
}

/// The signed patch list of `manifest`'s release, or `None` when the release
/// has none or it fails any check. Absence is normal, never an error.
pub async fn fetch(client: &reqwest::Client, manifest: &Manifest, key: &str) -> Option<PatchList> {
    let url = |name: &str| reqwest::Url::parse(&asset_url(&manifest.version, name)).ok().filter(release_source::allows);
    let list = component_download::bounded(client, &url(LIST)?, LIST_LIMIT).await.ok()?;
    let signature = component_download::bounded(client, &url(&format!("{LIST}.sig"))?, 4096).await.ok()?;
    let signature = String::from_utf8(signature).ok()?;
    if let Err(error) = verify_before_install(&list, key, signature.trim()) {
        tracing::warn!(%error, "Ignoring a patch list with an invalid signature");
        return None;
    }
    let list: PatchList = serde_json::from_slice(&list).ok()?;
    match list.validate(manifest) {
        Ok(()) => Some(list),
        Err(error) => {
            tracing::warn!(%error, "Ignoring an invalid patch list");
            None
        }
    }
}

/// Downloads and applies `patch` to `base`, writing `destination`, which must
/// then match `target` from the signed manifest. Leaves no file on failure.
pub async fn apply(
    client: &reqwest::Client, patch: &Patch, base: &Path, target: &RuntimeFile, destination: &Path, gate: &dyn UpdateGate,
) -> Result<(), UpdateError> {
    wait_until_allowed(gate).await?;
    let url = reqwest::Url::parse(&patch.url).map_err(|_| invalid("Invalid patch URL"))?;
    if !release_source::allows(&url) { return Err(invalid("Patch URL is not an allowed release source")); }
    let bytes = component_download::bounded(client, &url, patch.size as usize).await?;
    if bytes.len() as u64 != patch.size || format!("{:x}", Sha256::digest(&bytes)) != patch.sha256 {
        return Err(invalid("Patch size or digest does not match the signed list"));
    }
    let metadata = fs::symlink_metadata(base)?;
    if !metadata.is_file() || metadata.len() > FILE_LIMIT { return Err(invalid("Patch base is not a regular runtime file")); }
    let old = fs::read(base)?;
    let output = decompress(&old, &bytes, target.size)?;
    let written = (|| {
        let mut file = fs::OpenOptions::new().write(true).create_new(true).open(destination)?;
        file.write_all(&output)?;
        file.sync_all()?;
        drop(file);
        verify_file(destination, target)
    })();
    if written.is_err() { let _ = fs::remove_file(destination); }
    written
}

/// Reconstructs exactly `size` bytes; a patch producing more or less fails.
pub fn decompress(old: &[u8], patch: &[u8], size: u64) -> Result<Vec<u8>, UpdateError> {
    let mut context = DCtx::create();
    context.set_parameter(DParameter::WindowLogMax(WINDOW_LOG)).map_err(zstd_error)?;
    context.ref_prefix(old).map_err(zstd_error)?;
    let mut output = vec![0_u8; usize::try_from(size).map_err(|_| invalid("Patch target too large"))?];
    let written = context.decompress(&mut output[..], patch).map_err(zstd_error)?;
    if written as u64 != size { return Err(invalid("Patch produced the wrong size")); }
    Ok(output)
}

/// The release tool's side: a patch from `old` to `new`.
pub fn create(old: &[u8], new: &[u8]) -> Result<Vec<u8>, UpdateError> {
    let mut context = CCtx::create();
    for parameter in [CParameter::CompressionLevel(LEVEL), CParameter::EnableLongDistanceMatching(true), CParameter::WindowLog(WINDOW_LOG)] {
        context.set_parameter(parameter).map_err(zstd_error)?;
    }
    context.ref_prefix(old).map_err(zstd_error)?;
    let mut patch = vec![0_u8; zstd::zstd_safe::compress_bound(new.len())];
    let written = context.compress2(&mut patch[..], new).map_err(zstd_error)?;
    patch.truncate(written);
    Ok(patch)
}

fn zstd_error(code: usize) -> UpdateError { invalid(format!("zstd: {}", zstd::zstd_safe::get_error_name(code))) }

pub fn sha256_file(path: &Path) -> std::io::Result<String> {
    let mut file = fs::File::open(path)?;
    let mut hash = Sha256::new();
    let mut buffer = vec![0; 1024 * 1024];
    loop {
        let read = file.read(&mut buffer)?;
        if read == 0 { break; }
        hash.update(&buffer[..read]);
    }
    Ok(format!("{:x}", hash.finalize()))
}

/// Release tooling: patches from each earlier release under `previous`
/// (one directory per version, files named as release assets) to the new
/// runtime under `runtime`, plus the unsigned `patches-v1.json` in `out`.
/// Skips a patch that would not be meaningfully smaller than the file.
pub fn write_runtime_patches(runtime: &Path, version: &str, previous: &Path, out: &Path) -> Result<usize, UpdateError> {
    fs::create_dir_all(out)?;
    let mut patches = Vec::new();
    let mut releases: Vec<_> = fs::read_dir(previous)?.flatten().filter(|entry| entry.path().is_dir()).collect();
    releases.sort_by_key(|entry| entry.file_name());
    for release in releases {
        for path in PATHS {
            let old_path = release.path().join(path.rsplit('/').next().unwrap_or(path));
            let (Ok(old), Ok(new)) = (fs::read(&old_path), fs::read(runtime.join(path))) else { continue };
            if old == new { continue; }
            let patch = create(&old, &new)?;
            if patch.len() as u64 * 10 > new.len() as u64 * 9 { continue; }
            let from_sha256 = format!("{:x}", Sha256::digest(&old));
            if patches.iter().any(|existing: &Patch| existing.path == *path && existing.from_sha256 == from_sha256) { continue; }
            let name = file_name(path, &from_sha256);
            fs::write(out.join(&name), &patch)?;
            patches.push(Patch {
                path: (*path).to_owned(), from_sha256, sha256: format!("{:x}", Sha256::digest(&patch)),
                size: patch.len() as u64, url: asset_url(version, &name),
            });
        }
    }
    let list = PatchList { schema_version: 1, version: version.to_owned(), patches };
    fs::write(out.join(LIST), serde_json::to_vec_pretty(&list).map_err(|error| invalid(error.to_string()))?)?;
    Ok(list.patches.len())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn executable_like(seed: u8, length: usize) -> Vec<u8> {
        (0..length).map(|index| ((index * 7919) % 251) as u8 ^ if index % 4096 == 0 { seed } else { 0 }).collect()
    }

    #[test]
    fn patch_reconstructs_the_new_file_and_is_tiny() {
        let old = executable_like(1, 3 << 20);
        let mut new = old.clone();
        new[1_000_000..1_000_064].fill(0xAB);
        new.extend_from_slice(b"new code");
        let patch = create(&old, &new).unwrap();
        assert!(patch.len() < 4096, "patch was {} bytes", patch.len());
        assert_eq!(decompress(&old, &patch, new.len() as u64).unwrap(), new);
    }

    #[test]
    fn wrong_base_wrong_size_and_corruption_never_produce_the_target() {
        let old = executable_like(1, 1 << 20);
        let mut new = old.clone();
        new[4096..4200].fill(9);
        let patch = create(&old, &new).unwrap();
        let other = executable_like(2, 1 << 20);
        assert_ne!(decompress(&other, &patch, new.len() as u64).ok(), Some(new.clone()));
        assert!(decompress(&old, &patch, new.len() as u64 - 1).is_err(), "output larger than signed size is refused");
        assert!(decompress(&old, &patch, new.len() as u64 + 1).is_err(), "output smaller than signed size is refused");
        let mut corrupt = patch.clone();
        let middle = corrupt.len() / 2;
        corrupt[middle] ^= 0xFF;
        assert_ne!(decompress(&old, &corrupt, new.len() as u64).ok(), Some(new));
    }

    #[test]
    fn release_tool_lists_only_useful_patches_with_their_identities() {
        let root = tempfile::tempdir().unwrap();
        let runtime = root.path().join("runtime");
        let previous = root.path().join("previous");
        fs::create_dir_all(runtime.join("assets")).unwrap();
        fs::create_dir_all(previous.join("1.0.0")).unwrap();
        let old = executable_like(1, 2 << 20);
        let mut new = old.clone();
        new[10..20].fill(3);
        fs::write(previous.join("1.0.0/clipture.exe"), &old).unwrap();
        fs::write(runtime.join("clipture.exe"), &new).unwrap();
        fs::write(previous.join("1.0.0/ffmpeg.exe"), b"same").unwrap();
        fs::write(runtime.join("ffmpeg.exe"), b"same").unwrap();
        fs::write(previous.join("1.0.0/default.mp3"), b"aaaa").unwrap();
        fs::write(runtime.join("assets/default.mp3"), b"zzzz").unwrap();
        assert_eq!(write_runtime_patches(&runtime, "1.1.0", &previous, &root.path().join("out")).unwrap(), 1);
        let list: PatchList = serde_json::from_slice(&fs::read(root.path().join("out").join(LIST)).unwrap()).unwrap();
        let patch = &list.patches[0];
        assert_eq!(patch.path, "clipture.exe");
        assert_eq!(patch.from_sha256, format!("{:x}", Sha256::digest(&old)));
        let bytes = fs::read(root.path().join("out").join(file_name("clipture.exe", &patch.from_sha256))).unwrap();
        assert_eq!(format!("{:x}", Sha256::digest(&bytes)), patch.sha256);
        assert_eq!(decompress(&old, &bytes, new.len() as u64).unwrap(), new);
        assert_eq!(patch.url, format!("{}/v1.1.0/{}", release_source::asset_base(), file_name("clipture.exe", &patch.from_sha256)));
    }
}
