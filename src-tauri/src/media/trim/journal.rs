//! The trim journal: the whole plan, written and flushed before the clip's
//! first byte changes (the commit point), plus progress after each block.
//!
//! ```text
//! 0      header   "CLPTRIM1", plan length, BLAKE3 of the plan
//! 64     slot A   progress, with its own BLAKE3
//! 160    slot B   progress (written alternately with A, so a torn write
//!                 of one leaves the other)
//! 4096   plan
//! ...    staging  one block, for moves onto their own source
//! ```
//!
//! A journal whose header or plan does not verify was never committed: the
//! clip is untouched and the journal is simply discarded.
use std::{io, path::PathBuf};

use super::disk::Disk;
use super::layout::{Plan, Run};

const MAGIC: &[u8; 8] = b"CLPTRIM1";
const PLAN_AT: u64 = 4096;
const SLOT_AT: [u64; 2] = [64, 160];
const SLOT_LEN: usize = 96;
/// Plans beyond this are refused when read back (an index is a few MB).
const MAXIMUM_PLAN_BYTES: u64 = 256 * 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Progress {
    pub(super) seq: u64,
    /// Blocks before this one are done and durable.
    pub(super) next_block: u64,
    /// `next_block` was copied to the staging area: its length and digest.
    pub(super) staged: Option<(u64, [u8; 32])>,
}

pub(super) fn staging_at(plan_len: u64) -> u64 {
    PLAN_AT + plan_len.div_ceil(4096) * 4096
}

/// Writes the plan and first progress record, then the header that commits
/// them. Nothing in the clip may change before this returns.
pub(super) fn commit(journal: &mut impl Disk, plan: &Plan) -> io::Result<(u64, Progress)> {
    let encoded = encode(plan);
    journal.write_at(PLAN_AT, &encoded)?;
    let progress = Progress { seq: 1, next_block: 0, staged: None };
    write_slot(journal, &progress)?;
    journal.sync()?;
    let mut header = [0_u8; 64];
    header[..8].copy_from_slice(MAGIC);
    header[8..16].copy_from_slice(&(encoded.len() as u64).to_be_bytes());
    header[16..48].copy_from_slice(blake3::hash(&encoded).as_bytes());
    journal.write_at(0, &header)?;
    journal.sync()?;
    Ok((encoded.len() as u64, progress))
}

/// The committed plan and latest progress; `None` for an uncommitted journal.
pub(super) fn read(journal: &mut impl Disk) -> io::Result<Option<(Plan, u64, Progress)>> {
    let len = journal.len()?;
    if len < PLAN_AT {
        return Ok(None);
    }
    let mut header = [0_u8; 64];
    journal.read_at(0, &mut header)?;
    let plan_len = u64::from_be_bytes(header[8..16].try_into().unwrap());
    if &header[..8] != MAGIC || plan_len > MAXIMUM_PLAN_BYTES || PLAN_AT + plan_len > len {
        return Ok(None);
    }
    let mut encoded = vec![0_u8; plan_len as usize];
    journal.read_at(PLAN_AT, &mut encoded)?;
    if blake3::hash(&encoded).as_bytes() != &header[16..48] {
        return Ok(None);
    }
    let plan = decode(&encoded).ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "trim journal plan is unreadable"))?;
    let mut latest: Option<Progress> = None;
    for at in SLOT_AT {
        let mut slot = [0_u8; SLOT_LEN];
        journal.read_at(at, &mut slot)?;
        if blake3::hash(&slot[..64]).as_bytes() != &slot[64..] {
            continue;
        }
        let word = |index: usize| u64::from_be_bytes(slot[index * 8..index * 8 + 8].try_into().unwrap());
        let staged_len = word(3);
        let progress = Progress {
            seq: word(0),
            next_block: word(1),
            staged: (word(2) == 1).then(|| (staged_len, slot[32..64].try_into().unwrap())),
        };
        if latest.is_none_or(|current| progress.seq > current.seq) {
            latest = Some(progress);
        }
    }
    let progress = latest.ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "trim journal progress is unreadable"))?;
    Ok(Some((plan, plan_len, progress)))
}

/// Records progress durably, in the slot the previous record did not use.
pub(super) fn write_slot(journal: &mut impl Disk, progress: &Progress) -> io::Result<()> {
    let mut slot = [0_u8; SLOT_LEN];
    slot[..8].copy_from_slice(&progress.seq.to_be_bytes());
    slot[8..16].copy_from_slice(&progress.next_block.to_be_bytes());
    if let Some((len, digest)) = progress.staged {
        slot[16..24].copy_from_slice(&1_u64.to_be_bytes());
        slot[24..32].copy_from_slice(&len.to_be_bytes());
        slot[32..64].copy_from_slice(&digest);
    }
    let digest = blake3::hash(&slot[..64]);
    slot[64..].copy_from_slice(digest.as_bytes());
    journal.write_at(SLOT_AT[(progress.seq % 2) as usize], &slot)?;
    journal.sync()
}

fn encode(plan: &Plan) -> Vec<u8> {
    let mut out = Vec::with_capacity(plan.moov.len() + plan.runs.len() * 24 + 512);
    let mut put = |value: u64| out.extend(value.to_be_bytes());
    put(1); // version
    let path = plan.clip.to_string_lossy().into_owned().into_bytes();
    put(path.len() as u64);
    out.extend(&path);
    for value in [plan.clip_len, plan.block_bytes, plan.mdat_header_at, plan.moov_at, plan.final_len, plan.duration_ms] {
        out.extend(value.to_be_bytes());
    }
    out.extend((plan.mdat_header.len() as u64).to_be_bytes());
    out.extend(&plan.mdat_header);
    out.extend((plan.moov.len() as u64).to_be_bytes());
    out.extend(&plan.moov);
    out.extend((plan.runs.len() as u64).to_be_bytes());
    for run in &plan.runs {
        for value in [run.src, run.dst, run.len] {
            out.extend(value.to_be_bytes());
        }
    }
    out
}

fn decode(bytes: &[u8]) -> Option<Plan> {
    let mut reader = Reader { bytes, at: 0 };
    if reader.word()? != 1 {
        return None;
    }
    let path_len = reader.len()?;
    let clip = PathBuf::from(String::from_utf8(reader.take(path_len)?).ok()?);
    let fixed: Vec<u64> = (0..6).map(|_| reader.word()).collect::<Option<_>>()?;
    let header_len = reader.len()?;
    let mdat_header = reader.take(header_len)?;
    let moov_len = reader.len()?;
    let moov = reader.take(moov_len)?;
    let run_count = reader.len()?;
    if run_count > bytes.len() / 24 {
        return None;
    }
    let runs = (0..run_count)
        .map(|_| Some(Run { src: reader.word()?, dst: reader.word()?, len: reader.word()? }))
        .collect::<Option<Vec<_>>>()?;
    (reader.at == bytes.len()).then_some(Plan {
        clip,
        clip_len: fixed[0],
        block_bytes: fixed[1],
        mdat_header_at: fixed[2],
        moov_at: fixed[3],
        final_len: fixed[4],
        duration_ms: fixed[5],
        mdat_header,
        moov,
        runs,
    })
}

struct Reader<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl Reader<'_> {
    fn word(&mut self) -> Option<u64> {
        let value = u64::from_be_bytes(self.bytes.get(self.at..self.at + 8)?.try_into().ok()?);
        self.at += 8;
        Some(value)
    }

    fn len(&mut self) -> Option<usize> {
        usize::try_from(self.word()?).ok()
    }

    fn take(&mut self, len: usize) -> Option<Vec<u8>> {
        let value = self.bytes.get(self.at..self.at.checked_add(len)?)?.to_vec();
        self.at += len;
        Some(value)
    }
}
