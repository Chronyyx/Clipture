# Replay storage and low-copy saves: architecture report

Date: 2026-09-09. Status: proposal, not an accepted ADR or implementation.

Historical design baseline: the user's subsequent direction prioritizes default-on
in-place saves without overlapping presentation windows. Current implementation,
verification and limitations are in [milestone 6](replay-storage-progress.md).
Descriptions of current code below refer to the original report date.

## Executive recommendation

Keep independent, ordinary MP4 clips as the product contract. Do not replace them
with clip folders, reference movies, WTV, or a custom player-only format.

There is a credible improvement path: store encoded media in an MP4-ready layout
before Save, retain a small bounded RAM working set, and select the publishing
strategy by actual filesystem capability. On supported ReFS volumes, block
cloning could eliminate most payload copying. On ordinary NTFS, retain a bounded
copy fallback. Separately prototype a single-file rolling MP4 buffer that can be
finalized in place; do not promise that path before compatibility and overlapping
save tests pass.

Moving temporary files into the saving directory alone is not the optimization.
The opportunity is to change their byte layout and ownership. Also, the current
code already avoids a late category-folder copy when hard links succeed.

This report is based on source inspection and primary documentation. No recording,
settings, cache, installation, or user clips were changed. No performance or
filesystem-capability benchmark was run; none of the speedups below is measured.

## 1. What Clipture actually does today

| Stage | Implementation evidence | Consequence |
| --- | --- | --- |
| Replay RAM policy | [Engine.cpp](../engine/src/Engine.cpp), `replayMemoryBudgets` | Memory-aware video/AAC budget; not a full-window RAM requirement. |
| Temporary storage | [ReplaySegmentStore.cpp](../engine/src/ReplaySegmentStore.cpp), `defaultReplayRoot`, `openSegment` | Defaults to `%LOCALAPPDATA%\Clipture\ReplayCache`; separate video/audio/PCM-recovery session directories containing `.bin` files. |
| Spill and retirement | Same file, `scheduleSpillsLocked`, `writeEntry`, `trimLocked` | Old payloads spill when the resident budget is exceeded. Successful writes replace RAM payloads with readers. Expired packets retire; references protect files needed by outstanding snapshots. |
| Final MP4 construction | [Mp4Muxer.cpp](../engine/src/Mp4Muxer.cpp), `writeAvccSample`, `muxH264ToMp4` | Reads selected bytes, converts H.264 Annex B framing to MP4 length-prefixed samples, and writes a new MP4 under `saveFolder`. Video is not re-encoded in this mux path. |
| Optional postprocessing | [save_processor.rs](../src-tauri/src/media/save_processor.rs), `process`, `stitch` | Scaling, audio remapping/mixing, or resolution-changing clips can require another output. Some branches re-encode. |
| Final category publication | [finalize.rs](../src-tauri/src/clips/finalize.rs), `publish_saved` | First attempts a hard link, then deletes the staging name after publication. Copy-and-sync fallback exists. The common successful hard-link path already avoids another clip-sized copy. |

Video spill segments target eight seconds or 64 MiB, rotating on a keyframe;
audio segments target 30 seconds or 8 MiB. These are targets, not exact limits.
The engine retains decoder preroll before the visible start and already emits
MP4 edit lists for the requested presentation window.

The normal computed video-plus-AAC cache cap is approximately 64–192 MiB,
depending on available memory, with a 160 MiB fallback when the memory query
fails. Active PCM recovery has a separate allowance. This is not the whole
engine's RAM limit: capture surfaces, encoder resources, packet metadata,
snapshots, audio processing, and output staging are additional. Mux staging alone
currently ranges from 512 KiB for resident sources to 4/16/64 MiB for disk-backed
sources on non-seeking/unknown/seeking storage respectively.

Existing tests cover disk-backed packet reads, snapshot retention, RAM fallback,
oldest-first spill, and mux structure. The synthetic mux test checks for an edit
list; it does not establish playback compatibility for a new storage layout.

## 2. Corrections to the earlier discussion

- DVR is a recording/time-shift system, not an inherently faster codec or one
  universal file extension. We already have hybrid replay storage.
- Exact 120-second presentation does not require 120 seconds in RAM. Our current
  MP4 muxer already distinguishes decoder preroll from the visible interval.
- A saved MP4 need not always re-encode its first GOP to present an exact start:
  edit metadata can retain hidden decoder preroll. This is not the same as
  physically removing every compressed sample outside the visible window.
- "A circular buffer can never become a normal MP4 without copying" was too
  absolute. An appropriately designed media area and sample index make in-place
  finalization a plausible prototype. Interoperability remains to be proven.
- Sharing completed-file names with a hard link is not a snapshot of a live
  file: subsequent changes are visible through both names. Do not hard-link a
  writable ring buffer and then overwrite its saved footage. [Windows hard links](https://learn.microsoft.com/en-us/windows/win32/fileio/hard-links-and-junctions)

## 3. Where the bytes and time go

Illustration only: 120 seconds at 50 Mb/s is about **750 MB of video**, before
audio, metadata, and preroll. Continuous disk-first recording at that bitrate
issues roughly **22.5 GB of video writes per hour**, even when nothing is saved.

For a wholly disk-backed 750 MB window, the present mux path logically reads
about that payload and writes roughly another 750 MB. Windows caching changes
physical I/O, not the fact that a new output is constructed. A RAM-resident tail
does not incur that source disk read. On small windows that fit in the RAM cache,
disk-first storage can increase total writes compared with today.

Same folder does not mean adjacent SSD blocks. Same volume matters for linking,
renaming, and cloning. A single HDD serving both buffer reads and final writes
may also suffer seek contention; putting everything together is not always
faster than a separate cache SSD.

We must measure the entire Save path: configuration, selection, audio catch-up
and repair, mux reads/writes, pacing, optional FFmpeg processing, and publication.
Existing `[save-timing]` logs and the one-shot I/O analyzer provide a starting
point. Their process-level counters are not equivalent to physical SSD writes,
and concurrent capture can affect them. No evidence here establishes that copying
is the dominant cause of the user's previously observed long saves.

## 4. Options compared

| Option | Final artifact | Payload work on Save | Assessment |
| --- | --- | --- | --- |
| Smaller RAM cache, existing storage | Ordinary MP4 | Existing mux copy remains | Lowest-risk RAM experiment; more continuous disk I/O. |
| MP4-ready disk chunks + copy writer | Ordinary MP4 | Usually one sequential payload copy | Portable foundation; moves packet formatting earlier but does not eliminate writes. |
| MP4-ready aligned chunks + ReFS cloning | Independent MP4 | Metadata operations for cloneable ranges; copy remaining bytes | Best documented route to low-copy overlapping saves, but filesystem-limited. |
| Single rolling media file, finalized in place | Intended ordinary MP4 | Mostly metadata for eligible saves | Worth an isolated experiment; complex continuation and compatibility constraints. |
| Keep referenced chunks as the saved clip | Manifest plus backing data | Small metadata commit | Works architecturally, but conflicts with the user's ordinary-file preference. Not recommended. |
| OBS-style append-only hybrid MP4 | Ordinary finalized MP4 | Small finalization changes | Good for normal recording; alone it does not bound a rolling 120-second history. |

OBS demonstrates finalizing already-written fragments by adding a full movie
index and changing a small header, rather than copying the whole recording.
Its published design is append-only recording, not a rolling overwrite design.
We can borrow the principle, not assume the rolling extension is solved.
[OBS hybrid MP4 design](https://obsproject.com/blog/obs-studio-hybrid-mp4)

## 5. Recommended common foundation

Proposed ownership and data flow:

```text
C++ encoder / live audio
  -> MP4 sample packer
  -> bounded RAM write queue
  -> immutable, MP4-ready disk extents + timestamp index
  -> save-window snapshot (pins required extents)
       -> ordinary copy writer, or
       -> capability-gated ReFS clone writer
  -> validated, finalized MP4
Rust media processing, if needed -> Rust publication + library commit
```

"Extent" here means a recorded byte range with a known file, offset, and length.
It is internal implementation data, not a new user-visible clip format.

### Record once in the useful byte layout

Convert H.264 Annex B framing into MP4 length-prefixed samples before persistence.
Record sample lengths, timestamps, keyframes, codec configuration/epoch, and
logical audio identity alongside the payload. This is packet formatting, not
video re-encoding. Preserve the existing H.264 analyzer's behavior and fallback
until the new path passes parity tests.

Group samples into immutable, reasonably large aligned regions, rather than
padding every video frame or small AAC packet to a disk allocation unit. Keep
alignment padding outside referenced sample lengths. Account for it explicitly.
Finalize partial tails on demand; do not wait for an entire eight-second segment
before a user can save.

Start with the current codecs and recording semantics. HEVC is a separate project.

### Small, explicitly bounded RAM working set

Try 8, 16, and 32 MiB encoded-write-queue budgets in an isolated benchmark; these
are candidate settings, not selected production defaults. At 50 Mb/s, 16 MiB
holds approximately 2.7 seconds of video before audio overhead. That cushions a
brief stall but cannot mask a persistently slow or unavailable disk.

Do not hold capture/encoder locks across disk I/O. Define queue limits, error
states, and bounded emergency retention. If storage cannot keep up, preserve
already accepted saves, report degradation, and never silently grow RAM without
limit or claim a full replay window that no longer exists.

### Cache placement is a policy, not just a path

For the ReFS fast path, cache and destination must share the same volume. A private
cache directory below the selected save root is one option. On ordinary storage,
retain the option of a separate fast cache drive. Do not relocate user data or
start filling a synced folder without an explicit setting/decision.

Exclude private cache files from library discovery. Validate canonical paths and
reparse-point boundaries. Existing sessions keep their original placement when
the user changes save folders; do not move live referenced extents underneath them.

## 6. ReFS block-clone publishing

Windows documents block cloning as a ReFS facility, not a general NTFS feature.
It gives separate files independent write behavior while sharing underlying data.
Newer Windows copy APIs may use it automatically, but our custom muxer cannot
assume ordinary `WriteFile` calls will assemble a cloned MP4.
[Windows block cloning](https://learn.microsoft.com/en-us/windows/win32/fileio/block-cloning)

Proposed algorithm:

1. Freeze the save's sample index and pin its backing regions.
2. Create a private output on the same eligible volume; reserve metadata space
   and extend its logical length before cloning.
3. Clone complete aligned media regions. Generate new sample offsets pointing
   into those regions; write required boundary samples or repairs normally.
4. Write the final movie index, validate, flush under an explicit durability
   policy, and publish without replacing any existing clip.
5. Release pins only after publication/recovery ownership is established.

ReFS requires cluster-aligned ranges, same-volume source/destination, compatible
integrity settings, and additional limits. Query the actual cluster size rather
than assuming 4 KiB. Keep every clone request below 4 GB and handle sparse-file
requirements. A clone failure must safely fall back to a fresh copy output.
[ReFS restrictions](https://learn.microsoft.com/en-us/windows-server/storage/refs/block-cloning)

Original Annex B packet files cannot just be cloned into our present MP4 writer:
their bytes change during muxing. Likewise, tightly packing output samples at
arbitrary offsets would defeat alignment. The new layout and output planner are
necessary. Measure bytes cloned versus bytes copied, not just whether one clone
operation succeeded. Overlapping saves can share stable blocks without relying
on the original temporary filenames. Moving a completed clip elsewhere may
materialize its bytes; it remains a self-contained file.

No drive formatting, ReFS migration, or hard requirement on ReFS is proposed.

## 7. Experimental single-file finalization on NTFS

This is the closest match to the user's original idea, but has less evidence than
the clone path. Proposed scratch filename: `session-id.recording`, not a published
MP4 until validation succeeds.

Reserve a bounded media arena and a header region. Write MP4-ready video/audio
samples into reusable slots. Maintain a timestamp-to-offset index with generation
numbers so stale references can never read a newly overwritten slot. Reuse only
fully expired, unpinned regions and retain necessary decoder preroll.

On an eligible Save, stop writing to that arena, append a movie index describing
the selected samples, finalize the media header, and publish that same file as
`clip.mp4`. The payload would stay in place. MP4-family chunk tables describe file
offsets rather than requiring one tightly packed chronological byte stream; that
supports investigating this approach, not declaring it interoperable already.
[Chunk offsets](https://developer.apple.com/documentation/quicktime-file-format/chunk_offset_atom)

Unreferenced gaps could remain inside the media area. Sparse-file operations can
potentially release sufficiently large unused ranges without shifting retained
offsets. They do not compact the file's logical length. Zeroing an ordinary,
non-sparse file instead writes zeros and offers no such benefit.
[Sparse file operations](https://learn.microsoft.com/en-us/windows/win32/fileio/sparse-file-operations),
[zero-range behavior](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_zero_data)

### The continuation problem must not be hidden

After publishing, the old arena is immutable. New recording uses a new arena.
The logical replay index can still read the published file to preserve history;
it must not reset the user's buffer on Save.

If the user saves again ten seconds later, the new window includes about 110
seconds from the previous file and ten seconds from the new arena. That second
independent MP4 cannot be produced by simply renaming either file. It needs a
copy/clone fallback, or a different shared-storage product model. Continuously
duplicating data into a second ready-to-publish buffer merely shifts the copying
earlier; it does not remove it.

Moving/deleting/editing the published file while it still supplies replay history
introduces more risks. A retained hard link protects against deletion, not edits.
The first prototype must settle handle sharing, edit restrictions, replay lifetime,
and fallback behavior without silently compromising normal user file operations.

Other acceptance gates: wrapped offsets in Windows/WebView2/FFmpeg/editors,
file-size-versus-size-on-disk confusion, copying sparse gaps during sharing,
metadata reservation overflow, variable bitrate bursts, changing codecs/resolution,
late audio, and crash reconstruction of overwritten slots. A bounded arena avoids
an ever-growing sparse recording, but still needs a capacity/headroom policy.

Verdict: promising for some saves, not a universal instant-save guarantee. Keep
experimental and off by default until the ordinary-file experience is proven.

## 8. Exact duration, audio, and postprocessing

Use an explicit requested interval and distinguish it from the decoder interval.
Retain the preceding keyframe and necessary audio priming, then build presentation
metadata. Edit lists map movie time to media time; hiding preroll does not require
re-encoding the retained video. Validate the actual players we support rather
than treating the presence of `elst` as sufficient.
[Edit-list semantics](https://developer.apple.com/documentation/quicktime-file-format/playing_with_edit_lists)

Exact duration means a 120-second presentation timeline when enough history is
available, subject to real frame/sample timing. It does not promise 120 seconds
of newly captured frames if capture stalled, or physically zero extra decoder data.
Hidden preroll is also not secure deletion of out-of-window content.

Preserve separate audio identities, gaps, codec epochs and priming. Current audio
repair/mixing and resolution normalization are real behavior, not optional work
to discard for speed. Fast-path eligibility initially requires stable video
configuration and an audio layout that does not need postprocessing. Otherwise
fall back to the existing pipeline. Later, investigate whether choosing the final
audio track ordering before muxing can remove some remap-only passes.

## 9. Reliability and source ownership

Suggested future C++ modules, split by responsibility rather than enlarging
`Engine.cpp` or `Mp4Muxer.cpp`:

- `replay/Mp4SamplePacker`: encoded sample representation.
- `replay/ReplayIndex`: windows, generations, pin lifetimes.
- `replay/ExtentStore`: immutable data regions and bounded write queue.
- `replay/ReplayJournal`: recoverable index/commit records and owned-file cleanup.
- `mux/Mp4LayoutPlanner`: sample offsets, edits, and final metadata.
- `mux/CopyPayloadWriter` and `platform/windows/ClonePayloadWriter`.
- Separate experimental `replay/RollingMp4Arena`; not intertwined with the fallback.

Rust remains responsible for validated placement policy, postprocessing,
publication/library commit, and notifications. The renderer owns none of this.
Cache paths and capability flags crossing the engine boundary require coordinated
protocol types, fixtures, and tests. Avoid a new renderer storage API unless the
user-facing settings actually need one.

Define save states: selected/pinned, writing, finalized, published, indexed.
Persist recovery intent before source data can be reclaimed. Do not announce
"saved" merely because a pin or filename exists. A user-space buffer flush or
successful close alone must not be described as proof of power-loss durability.
Specify and measure the actual file flush/commit ordering.

Handle disk-full, removed volumes, partial clone, process death, rename failure,
library-commit failure, and shutdown. Failed publication preserves recoverable
data; cleanup deletes only verified owned cache objects, never arbitrary contents
of `saveFolder`. No new recorder work may require a WebView to remain alive.

## 10. Proposed proof sequence and decision gates

1. **Baseline, not a rewrite.** Use the existing I/O analyzer plus host-stage
   timings on an isolated profile. Compare no-save steady state and repeated
   saves, including ten-second spacing. Attribute slow saves before optimizing.
2. **Portable MP4-ready layout experiment.** Synthetic/replayed encoded input,
   separate test output. Verify packet equivalence, duration, audio alignment,
   and memory bounds. Compare saved-file reads/writes against the current muxer.
3. **ReFS experiment, only if a suitable test volume is available.** Probe actual
   support with scratch files; measure clone coverage, alignment waste, logical
   and allocated bytes, and overlapping saves. No reformatting.
4. **Independent NTFS arena experiment.** Test wrapped physical ordering and
   metadata-only finalization, then sparse-space reclamation and immediate second
   saves. Reject or narrow the path if it breaks common players or normal file
   operations. Do not substitute clip manifests if that fails.
5. **Fault injection and integration.** Kill at each commit stage, remove a scratch
   destination, simulate full disk/slow writes, change resolution/audio layout,
   and close the UI while saving. Verify existing recording and library contracts.

Benchmark 30/120/600-second windows; representative 1080p60/1440p60/4K60 input;
static and high-motion footage; 0/1/multiple audio tracks; SSD/HDD; same/separate
volumes; warm/cold caches; and sustained versus burst saves. Track p50/p95/p99
request-to-playable and request-to-durable time, capture drops, queue peaks, total
process private memory, logical read/write bytes, physical I/O, and peak allocated
disk space. Report both successful fast paths and fallback frequency.

No claimed latency target or percentage saving is justified yet. Advance a path
only when it materially improves its intended metric without weakening capture,
audio, ordinary-file compatibility, or recoverability. The safe next step is a
small isolated prototype and measurements, not replacing the working recorder.
