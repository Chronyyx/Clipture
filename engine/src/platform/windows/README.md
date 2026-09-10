# Experimental payload clone backend

## Independent in-place file backend

`InPlaceFile.cpp` is separate from cloning: one `CREATE_NEW` handle owns a private
file through append, seal, header/index finalization, flush and no-replace
`FileRenameInfo` publication. Same-volume rename has no copy fallback. A name
collision leaves the finalized private file available for retry. Owned unused
archive files opt into deletion after their final lease. Detached save files opt
out of deletion for recovery, including after failures.

Only committed media ranges at/above byte 4096 are exposed to payload readers.
The reservation and appended movie index are not public source extents; `size()`
is the media-end bound, not the final file length. All I/O uses the owned handle
and its mutex. Publication obtains a read-only `ReOpenFile` handle by file identity
before renaming, then drops the exclusive writer so the host can move/unlink its
staging path while decoder leases remain. No path reopening or file replacement.
Sealed files permit only explicit supplemental-sample append and finalization;
all writes are forbidden after publication. Range reuse is governed by archive leases.

Logical counters do not measure physical disk writes. Final flush can wait for
previously buffered media. Crash recovery, volume-crossing fallback and live
capture-aware pacing are not implemented in this backend.

## Clone backend

This module is compiled but is not connected to live recording or production
save publication. It works on explicitly supplied sealed files and newly created
private destinations. No settings, library, cache discovery or UI dependencies.

## Ownership

- `CloneFileInfo.cpp`: query metadata from open handles; pure range eligibility
  and request-size policy. Requires ReFS plus the block-refcounting capability,
  same GUID volume, distinct file IDs, known matching integrity settings and
  cluster size, valid aligned ranges, and an already-extended destination.
  Sparse/compressed/encrypted/reparse/offline inputs conservatively use copying.
- `SealedPayloadFile.cpp`: read-only source handle held for the pin lifetime,
  denying writes/deletion. Only open application-owned, already-sealed files;
  this is not a general snapshot primitive or a live append-buffer adapter.
- `WindowsPayloadOutput.cpp`: exclusive `CREATE_NEW` destination, length setup,
  bounded writes, actual `FSCTL_DUPLICATE_EXTENTS_TO_FILE` requests, flush/close,
  and deletion by owned handle. Clone requests are aligned and at most 1 GiB,
  strictly below the documented 4 GiB limit. No existing file is overwritten.
- `replay/PayloadCloneJob.cpp`: host-neutral attempt lifecycle. Unsupported
  ranges copy normally. Any clone-operation failure discards the entire output
  and retries once in a fresh file with cloning disabled. A failed discard stops
  the job. Read/write/finish failures are not reported as saved or retried.

`transferPayload` reports copied media, cloned media, zero padding, unsupported
ranges and clone failures separately. `copyPayload` remains copy-only. Sources
stay pinned across retry. Factory outputs must clean up unfinished attempts on
destruction, including when an exception propagates.

Successful `writePayloadFile` means the requested payload region was written and
the staging handle flushed/closed. It does not build missing MP4 headers, publish
to the library, journal recovery intent, or prove power-loss durability. Prefix
space outside the planned region is not a valid movie header. Caller owns those
steps; do not announce a clip saved based only on this API.

## Verification limits and next integration

This PC's mounted C:/D:/E: volumes are NTFS. Real capability checks, copy fallback,
collision protection, source pinning, cleanup, and output independence passed.
Mock clone success/failure and actual scratch-file discard/recreate fault injection
passed, but **no successful ReFS clone was exercised**. Tests print that skip
explicitly; a passing fallback probe is not evidence of clone support.

The media fixture copies/clones an already completed synthetic MP4 as a sealed
file. It validates this backend's byte preservation, not live MP4 assembly.
Live `SegmentBacking` currently provides only generic extents and is intentionally
unsupported by this cloner. Integration must expose verified closed-segment
handles without reopening mutable user files, retain the existing save I/O pacing,
coordinate header/index regeneration and fallback, and preserve audio behavior.
Do not replace the capture-aware mux writer with this standalone file writer.

Before enabling: successful real ReFS probe and independent-write test, live
segment lifetime/overlap coverage, complete mux A/V parity, publication recovery,
and performance measurements. No formatting, cache relocation or production
default changes are part of this module.

Primary contracts: [block-clone restrictions](https://learn.microsoft.com/en-us/windows-server/storage/refs/block-cloning),
[clone IOCTL](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_duplicate_extents_to_file),
[volume capability flags](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumeinformationbyhandlew),
[integrity/cluster information](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-fsctl_get_integrity_information_buffer).
