//! Staging with binary patches: a valid signed patch is used instead of a
//! download, and anything wrong with the patch path falls back to one.
use super::{stage, transaction::Paths};
use crate::updates::{
    component_download::tests::TestServer,
    manifest::{Components, Compatibility, Manifest, RuntimeFile, SignedManifest, PATHS},
    patches::{self, Patch, PatchList},
    release_source::TEST_ASSET_BASE,
    service::AllowUpdates,
};
use base64::Engine;
use sha2::{Digest, Sha256};
use std::fs;

const VERSION: &str = "9.9.9";

fn sha(bytes: &[u8]) -> String { format!("{:x}", Sha256::digest(bytes)) }

fn sign(keys: &minisign::KeyPair, bytes: &[u8]) -> String {
    let signature = minisign::sign(Some(&keys.pk), &keys.sk, bytes, None, None).unwrap();
    base64::engine::general_purpose::STANDARD.encode(signature.to_string())
}

fn old_controller() -> Vec<u8> { (0..(2 << 20)).map(|index: usize| ((index * 7919) % 251) as u8).collect() }

fn new_controller() -> Vec<u8> {
    let mut bytes = old_controller();
    bytes[700_000..700_100].fill(0x5A);
    bytes
}

struct Release { key: String, keys: minisign::KeyPair, signed: SignedManifest, base: String, server: TestServer }

/// A signed manifest whose controller changed and whose other files are
/// already installed. The full controller is only served if `serve_full`.
fn release(serve_full: bool) -> Release {
    let server = TestServer::start();
    let probe = server.serve("probe", b"x".to_vec(), 1);
    let base = format!("{}releases", probe.trim_end_matches("probe"));
    let keys = minisign::KeyPair::generate_unencrypted_keypair().unwrap();
    let key = base64::engine::general_purpose::STANDARD.encode(keys.pk.to_box().unwrap().to_string());
    let files: Vec<RuntimeFile> = PATHS.iter().map(|path| {
        let bytes = if *path == "clipture.exe" { new_controller() } else { b"same".to_vec() };
        let name = path.rsplit('/').next().unwrap();
        let url = format!("{base}/v{VERSION}/{name}");
        if *path == "clipture.exe" && serve_full { server.serve(&format!("releases/v{VERSION}/{name}"), bytes.clone(), 1); }
        RuntimeFile {
            path: (*path).into(), size: bytes.len() as u64, sha256: sha(&bytes), url,
            blocks: bytes.chunks(1_048_576).map(sha).collect(), block_size: 1_048_576,
        }
    }).collect();
    let engine = files.iter().find(|file| file.path == "clipture_engine.exe").unwrap().sha256.clone();
    let manifest = serde_json::to_string(&Manifest {
        schema_version: 1, version: VERSION.into(), platform: "windows-x86_64".into(),
        compatibility: Compatibility { host: engine.clone(), ui_protocol: 1, engine_protocol: 1 },
        components: Components { controller: engine.clone(), ui: engine.clone(), engine },
        files,
    }).unwrap();
    let signed = SignedManifest { signature: sign(&keys, manifest.as_bytes()), manifest };
    Release { key, keys, signed, base, server }
}

fn publish_patches(release: &Release, list: &PatchList, signature_over: Option<&[u8]>) {
    let bytes = serde_json::to_vec(list).unwrap();
    let signature = sign(&release.keys, signature_over.unwrap_or(&bytes));
    release.server.serve(&format!("releases/v{VERSION}/{}", patches::LIST), bytes, 1);
    release.server.serve(&format!("releases/v{VERSION}/{}.sig", patches::LIST), signature.into_bytes(), 1);
}

fn installed(root: &tempfile::TempDir) -> Paths {
    let paths = Paths::new(root.path().join("Clipture"));
    for path in PATHS {
        fs::create_dir_all(paths.installed(path).parent().unwrap()).unwrap();
        fs::write(paths.installed(path), if path == "clipture.exe" { old_controller() } else { b"same".to_vec() }).unwrap();
    }
    paths
}

fn controller_patch(release: &Release) -> (Patch, Vec<u8>) {
    let bytes = patches::create(&old_controller(), &new_controller()).unwrap();
    let from = sha(&old_controller());
    let name = patches::file_name("clipture.exe", &from);
    let patch = Patch { path: "clipture.exe".into(), from_sha256: from, sha256: sha(&bytes), size: bytes.len() as u64, url: format!("{}/v{VERSION}/{name}", release.base) };
    (patch, bytes)
}

fn stage_with(release: &Release, paths: &Paths) -> Result<super::journal::Journal, crate::updates::service::UpdateError> {
    TEST_ASSET_BASE.with(|base| *base.borrow_mut() = Some(release.base.clone()));
    let result = tauri::async_runtime::block_on(stage::stage(paths, &release.signed, &release.key, &AllowUpdates, None, |_| {}));
    TEST_ASSET_BASE.with(|base| *base.borrow_mut() = None);
    result
}

#[test]
fn signed_patch_replaces_the_download() {
    let release = release(false);
    let (patch, bytes) = controller_patch(&release);
    assert!(bytes.len() < 16 * 1024, "a 100-byte change patched in {} bytes", bytes.len());
    release.server.serve(&format!("releases/v{VERSION}/{}", patches::file_name("clipture.exe", &patch.from_sha256)), bytes, 1);
    publish_patches(&release, &PatchList { schema_version: 1, version: VERSION.into(), patches: vec![patch] }, None);
    let root = tempfile::tempdir().unwrap();
    let paths = installed(&root);
    let journal = stage_with(&release, &paths).expect("the full controller is not served, so only the patch can stage it");
    assert_eq!(journal.changed.len(), 1);
    assert_eq!(fs::read(paths.staged("clipture.exe")).unwrap(), new_controller());
}

#[test]
fn bad_patches_fall_back_to_the_normal_download() {
    let cases: [(&str, fn(&mut Patch, &mut Vec<u8>) -> bool); 4] = [
        ("unsigned list", |_, _| true),
        ("corrupt patch bytes", |_, bytes| { let middle = bytes.len() / 2; bytes[middle] ^= 0xFF; false }),
        ("patch from another base", |patch, _| { patch.from_sha256 = "0".repeat(64); false }),
        ("digest mismatch", |patch, _| { patch.sha256 = "1".repeat(64); false }),
    ];
    for (name, tamper) in cases {
        let release = release(true);
        let (mut patch, mut bytes) = controller_patch(&release);
        let forge_signature = tamper(&mut patch, &mut bytes);
        release.server.serve(&format!("releases/v{VERSION}/{}", patches::file_name("clipture.exe", &sha(&old_controller()))), bytes, 1);
        let list = PatchList { schema_version: 1, version: VERSION.into(), patches: vec![patch] };
        publish_patches(&release, &list, forge_signature.then_some(b"something else".as_slice()));
        let root = tempfile::tempdir().unwrap();
        let paths = installed(&root);
        stage_with(&release, &paths).unwrap_or_else(|error| panic!("{name}: fallback download failed: {error}"));
        assert_eq!(fs::read(paths.staged("clipture.exe")).unwrap(), new_controller(), "{name}");
    }
}

#[test]
fn a_release_without_patches_downloads_as_before() {
    let release = release(true);
    let root = tempfile::tempdir().unwrap();
    let paths = installed(&root);
    stage_with(&release, &paths).unwrap();
    assert_eq!(fs::read(paths.staged("clipture.exe")).unwrap(), new_controller());
}
