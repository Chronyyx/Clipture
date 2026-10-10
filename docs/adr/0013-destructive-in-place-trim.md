# ADR 0013: Destructive in-place trimming

Date: 2026-10-03

Status: accepted; the player's trim mode for Clipture's own clips.

## Context

Trimming should work like a phone's photo editor, except that the removed
footage is really gone: the clip's own file ends up holding only the kept
part, smaller on disk. Save in Place (`engine/src/replay/README.md`) makes
the replay arena itself the clip, so its samples are scattered through the
file with padding between them. Exporting a trimmed copy would need a second
copy of the clip on disk, and re-encoding would cost quality and time. Neither
is acceptable here, and a crash or power cut must never leave a broken clip.

## Decision

`media/trim` rewrites the clip in place, without re-encoding:

1. **Index.** `index.rs` keeps each video track's samples from the keyframe at
   or before the cut, and each audio track's from one packet before it (AAC
   priming). New edit lists start presentation exactly at the cut and end it
   exactly at the end point, so the result is frame accurate. Only one
   edit-list shape is accepted: leading empty edits, one media edit at normal
   speed, and trailing empty edits. Sample tables the trimmer cannot rewrite
   are refused, apart from AAC `roll` groups, `cslg` and `stps`, which are
   dropped.
2. **Layout.** `layout.rs` slides the kept samples down over the removed ones,
   keeping their order on disk. The media box shrinks to fit, the new index
   follows it, and the file is cut to length. No padding or holes remain.
   Because the order is kept, every move goes to a lower offset and never
   onto a kept byte that has not moved yet. The recorder's arena stores
   footage in recording order (next-fit placement, one wrap per lap), so a
   trimmed clip reads front to back too.
3. **Crash safety.** Before the first byte moves, `journal.rs` writes the
   whole plan to `%APPDATA%\Clipture\data\trims\*.trim` and flushes it. The
   plan covers the moves, the new index and the media header. A BLAKE3-checked
   header written last is the commit point. `apply.rs` then moves 16 MB blocks
   in plan order and records progress after each flushed block. Progress goes
   into two alternating, checksummed slots, so a torn write cannot lose it.
   - A block whose destination overlaps its own source is first copied to the
     journal's staging area and recorded as staged.
   - Redoing any block from the recorded progress is therefore safe: earlier
     blocks never write over a later block's source.
   - Finishing (index, media header, truncation) is repeatable from the journal.
4. **Outcome.** A trim stopped before the commit point leaves the clip
   untouched, and its journal is discarded. A trim stopped after it is
   finished at the next startup, before the library is read
   (`MediaService::finish_interrupted_trims`).
5. **Exclusive access.** The clip is opened with no sharing. The recorder keeps
   a just-saved clip open while it still copies footage from it, so a trim
   waits (up to 5 s) and then reports that the clip is busy instead of racing
   it. No player or thumbnailer can see a half-moved file. The handle asks
   for low I/O priority, so recording keeps the disk.

The extra disk used is the journal: the new index plus at most one block.
Imported videos and clips saved in several parts are not trimmed. Shares of
a trimmed clip end, because the file no longer matches what friends were
offered.

## Consequences

- A trim takes about a second for a two-minute 1440p clip on an NVMe drive. A
  347 MB clip trimmed to 45 s became 128 MB in 0.9 s, with every kept packet
  byte-identical and the first and last frames matching the original's at the
  cut points.
- Reordering a scattered clip during the trim was tried and dropped: it moved
  almost every kept byte, some twice, and took 13 s for that clip. Clips
  recorded before the next-fit arena keep their scattered order when
  trimmed; Clipture plays them in time order through the linear view
  (`media/linear_view.rs`).
- `power_loss_at_any_point_leaves_the_original_or_the_finished_trim` stops a
  trim at every write and flush (sampled for the larger trims). It simulates unflushed writes being lost,
  landing, or landing in part, and checks that recovery always yields the
  original or the exact finished trim.
- There is no undo. The player asks for confirmation before deleting.
