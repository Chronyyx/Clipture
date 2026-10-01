use super::{service::UpdateError, stream_download::verify_before_install};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{collections::BTreeSet, io::Read, path::Path};

pub const MANIFEST_LIMIT: usize = 256 * 1024;
pub const FILE_LIMIT: u64 = 512 * 1024 * 1024;
pub const PATHS: [&str; 5] = [
    "clipture.exe", "clipture_engine.exe", "ffmpeg.exe", "assets/default.mp3", "assets/option2.wav",
];

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Manifest {
    pub schema_version: u32,
    pub version: String,
    pub platform: String,
    pub compatibility: Compatibility,
    pub files: Vec<RuntimeFile>,
    pub components: Components,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Compatibility {
    pub host: String,
    pub ui_protocol: u32,
    pub engine_protocol: u32,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Components {
    pub controller: String,
    pub ui: String,
    pub engine: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct RuntimeFile {
    pub path: String,
    pub size: u64,
    pub sha256: String,
    pub url: String,
    pub blocks: Vec<String>,
    pub block_size: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SignedManifest {
    pub manifest: String,
    pub signature: String,
}

pub fn invalid(message: impl Into<String>) -> UpdateError {
    UpdateError::Transfer(message.into())
}

impl SignedManifest {
    pub fn verify(&self, key: &str) -> Result<Manifest, UpdateError> {
        if self.manifest.len() > MANIFEST_LIMIT || self.signature.len() > 4096 {
            return Err(invalid("Component manifest exceeds its size limit"));
        }
        verify_before_install(self.manifest.as_bytes(), key, self.signature.trim())?;
        let manifest: Manifest = serde_json::from_str(&self.manifest)
            .map_err(|error| invalid(format!("Invalid component manifest: {error}")))?;
        manifest.validate()?;
        Ok(manifest)
    }
}

impl Manifest {
    pub fn validate(&self) -> Result<(), UpdateError> {
        let version = semver::Version::parse(&self.version).map_err(|_| invalid("Invalid runtime version"))?;
        if self.schema_version != 1 || self.platform != "windows-x86_64"
            || !version.pre.is_empty() || !version.build.is_empty()
            || version.to_string() != self.version
            || self.compatibility.ui_protocol != 1 || self.compatibility.engine_protocol != 1
            || self.compatibility.host != self.components.controller
            || ![&self.components.controller, &self.components.ui, &self.components.engine].into_iter().all(|hash| valid_hash(hash))
        {
            return Err(invalid("Unsupported component schema, version, platform or compatibility"));
        }
        let mut paths = BTreeSet::new();
        let mut total = 0_u64;
        for file in &self.files {
            if !PATHS.contains(&file.path.as_str()) || !paths.insert(file.path.as_str())
                || file.size == 0 || file.size > FILE_LIMIT || !valid_hash(&file.sha256)
                || file.block_size != 1_048_576
                || file.blocks.len() as u64 != file.size.div_ceil(file.block_size)
                || !file.blocks.iter().all(|hash| valid_hash(hash))
            {
                return Err(invalid("Invalid runtime file, size, path, digest or block map"));
            }
            total += file.size;
            let name = file.path.rsplit('/').next().unwrap_or_default();
            let expected = format!("https://github.com/Chronyyx/Clipture/releases/download/v{}/{name}", self.version);
            let local_test = cfg!(test) && file.url.starts_with("http://127.0.0.1:");
            if file.url != expected && !local_test {
                return Err(invalid("Runtime file URL is not the expected release asset"));
            }
        }
        if paths.len() != PATHS.len() || total > 1024 * 1024 * 1024
            || self.files.iter().find(|file| file.path == "clipture_engine.exe").map(|file| &file.sha256) != Some(&self.components.engine)
        {
            return Err(invalid("Incomplete runtime or mismatched engine identity"));
        }
        Ok(())
    }

    pub fn newer_than(&self, version: &str) -> bool {
        semver::Version::parse(&self.version).ok().zip(semver::Version::parse(version).ok())
            .is_some_and(|(candidate, current)| candidate > current)
    }

    pub fn directory_name(&self) -> String {
        format!("{}-{}", self.version, self.files.iter().find(|file| file.path == "clipture.exe").unwrap().sha256)
    }
}

pub fn valid_hash(hash: &str) -> bool {
    hash.len() == 64 && hash.bytes().all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

pub fn verify_file(path: &Path, expected: &RuntimeFile) -> Result<(), UpdateError> {
    let metadata = std::fs::symlink_metadata(path)?;
    if !metadata.is_file() || metadata.len() != expected.size || is_reparse(&metadata) {
        return Err(invalid("Runtime file size/type does not match signed manifest"));
    }
    let mut file = std::fs::File::open(path)?;
    let mut hash = Sha256::new();
    let mut buffer = vec![0; 1024 * 1024];
    for (index, expected_block) in expected.blocks.iter().enumerate() {
        let remaining = expected.size - index as u64 * expected.block_size;
        let length = remaining.min(expected.block_size) as usize;
        file.read_exact(&mut buffer[..length])?;
        if format!("{:x}", Sha256::digest(&buffer[..length])) != *expected_block {
            return Err(invalid("Runtime block hash mismatch"));
        }
        hash.update(&buffer[..length]);
    }
    if format!("{:x}", hash.finalize()) != expected.sha256 {
        return Err(invalid("Runtime SHA256 mismatch"));
    }
    Ok(())
}

pub fn is_reparse(metadata: &std::fs::Metadata) -> bool {
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        metadata.file_attributes() & 0x400 != 0
    }
    #[cfg(not(windows))]
    metadata.file_type().is_symlink()
}

#[cfg(test)]
mod tests {
    use super::*;

    pub fn fixture() -> Manifest {
        let hash = format!("{:x}", Sha256::digest(b"test"));
        Manifest {
            schema_version: 1, version: "2.0.0".into(), platform: "windows-x86_64".into(),
            compatibility: Compatibility { host: hash.clone(), ui_protocol: 1, engine_protocol: 1 },
            components: Components { controller: hash.clone(), ui: hash.clone(), engine: hash.clone() },
            files: PATHS.iter().map(|path| RuntimeFile { path: (*path).into(), size: 4, sha256: hash.clone(),
                url: format!("https://github.com/Chronyyx/Clipture/releases/download/v2.0.0/{}", path.rsplit('/').next().unwrap()), blocks: vec![hash.clone()], block_size: 1_048_576 }).collect(),
        }
    }

    #[test]
    fn runtime_manifest_rejects_traversal_missing_duplicates_and_incompatibility() {
        assert!(fixture().validate().is_ok());
        for path in ["../clipture.exe", "C:/clipture.exe", "assets/../clipture.exe", "CLIPTURE.EXE", "clipture.exe:stream"] {
            let mut value = fixture(); value.files[0].path = path.into(); assert!(value.validate().is_err());
        }
        let mut value = fixture(); value.files.pop(); assert!(value.validate().is_err());
        let mut value = fixture(); value.files.push(value.files[0].clone()); assert!(value.validate().is_err());
        let mut value = fixture(); value.compatibility.ui_protocol = 2; assert!(value.validate().is_err());
        let mut value = fixture(); value.files[0].url.push_str("?other"); assert!(value.validate().is_err());
        assert!(fixture().newer_than("1.9.9")); assert!(!fixture().newer_than("2.0.0"));
    }

    #[test]
    fn signed_manifest_authorizes_exact_bytes_and_staged_runtime() {
        use base64::Engine;
        let keys = minisign::KeyPair::generate_unencrypted_keypair().unwrap();
        let key = base64::engine::general_purpose::STANDARD.encode(keys.pk.to_box().unwrap().to_string());
        let manifest = serde_json::to_string(&fixture()).unwrap();
        let signature = minisign::sign(Some(&keys.pk), &keys.sk, manifest.as_bytes(), None, None).unwrap();
        let signed = SignedManifest { manifest: manifest.clone(), signature: base64::engine::general_purpose::STANDARD.encode(signature.to_string()) };
        assert!(signed.verify(&key).is_ok());
        let root = tempfile::tempdir().unwrap();
        let directory = root.path().join(fixture().directory_name());
        std::fs::create_dir_all(directory.join("assets")).unwrap();
        for file in &fixture().files { std::fs::write(directory.join(&file.path), b"test").unwrap(); }
        super::super::runtime_store::atomic_signed(&directory, "manifest.json", &signed).unwrap();
        let verified = super::super::runtime_store::verify(root.path(), signed.clone(), &key).unwrap();
        super::super::runtime_store::atomic_signed(root.path(), "pending.json", &signed).unwrap();
        super::super::runtime_store::select(root.path(), &verified, &key).unwrap();
        drop(verified);
        assert!(super::super::runtime_store::load(root.path(), "active.json", &key).is_ok());
        let mut tampered = signed.clone(); tampered.manifest.push(' ');
        assert!(tampered.verify(&key).is_err());
        std::fs::write(directory.join("clipture_engine.exe"), b"Test").unwrap();
        assert!(super::super::runtime_store::load(root.path(), "active.json", &key).is_err());
    }

    #[test]
    fn staged_file_is_reauthenticated_and_tampering_fails() {
        let root = tempfile::tempdir().unwrap(); let path = root.path().join("clipture.exe");
        std::fs::write(&path, b"test").unwrap();
        assert!(verify_file(&path, &fixture().files[0]).is_ok());
        std::fs::write(&path, b"Test").unwrap();
        assert!(verify_file(&path, &fixture().files[0]).is_err());
    }
}
