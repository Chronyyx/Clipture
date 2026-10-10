//! Executes a committed plan. Safe to repeat from any recorded progress:
//!
//! - The plan only ever moves a block onto bytes nothing still needs, so a
//!   block's source is never overwritten by an earlier block. Redoing the
//!   block that was in flight reads an intact source.
//! - A block whose destination overlaps its own source would clobber the
//!   source as it is written. It is first copied to the journal's staging
//!   area and recorded as staged, so a redo writes it from there.
//! - Progress advances only after the clip is flushed.
//! - Finishing writes the new index and media header from the journal and
//!   cuts the file to length, all repeatable.
use std::io;

use super::disk::Disk;
use super::journal::{self, Progress};
use super::layout::Plan;

pub(super) fn run(
    clip: &mut impl Disk,
    journal_disk: &mut impl Disk,
    plan: &Plan,
    plan_len: u64,
    mut progress: Progress,
) -> io::Result<()> {
    let staging = journal::staging_at(plan_len);
    let blocks = plan.blocks();
    let mut buffer = Vec::new();
    for (index, block) in blocks.iter().enumerate().skip(progress.next_block as usize) {
        buffer.resize(block.len as usize, 0);
        let index = index as u64;
        match progress.staged.filter(|_| progress.next_block == index) {
            Some((len, digest)) => {
                // Staged before the crash: the source may already be partly
                // overwritten, so the staged copy is the only one to trust.
                journal_disk.read_at(staging, &mut buffer)?;
                if len != block.len || blake3::hash(&buffer).as_bytes() != &digest {
                    return Err(io::Error::new(io::ErrorKind::InvalidData, "the trim journal's staged block is damaged"));
                }
            }
            None => {
                clip.read_at(block.src, &mut buffer)?;
                if block.dst < block.src + block.len && block.src < block.dst + block.len {
                    journal_disk.write_at(staging, &buffer)?;
                    journal_disk.sync()?;
                    progress = Progress {
                        seq: progress.seq + 1,
                        next_block: index,
                        staged: Some((block.len, *blake3::hash(&buffer).as_bytes())),
                    };
                    journal::write_slot(journal_disk, &progress)?;
                }
            }
        }
        clip.write_at(block.dst, &buffer)?;
        clip.sync()?;
        progress = Progress { seq: progress.seq + 1, next_block: index + 1, staged: None };
        journal::write_slot(journal_disk, &progress)?;
    }
    clip.write_at(plan.moov_at, &plan.moov)?;
    clip.write_at(plan.mdat_header_at, &plan.mdat_header)?;
    clip.set_len(plan.final_len)?;
    clip.sync()
}
