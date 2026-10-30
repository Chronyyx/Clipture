# ADR 0026: Remove overlapping in-place saves

Date: 2026-10-30

Status: implemented; supersedes the `saveInPlaceOverlap` default from 1.5.1.

## Context

With Save in place, a save turns the live replay arena into the clip file
without copying media. With `saveInPlaceOverlap` on (the default since 1.5.1),
the next save again covered the full clip length, so the footage both clips
share had to be copied back from the saved clip into a new arena: in the
background at 1.1x the footage rate, and whatever was left synchronously inside
the next save, on the engine's request thread, at the lowest I/O priority.

Measured on a 4 TB SMR hard drive (Seagate ST4000DM004) with an isolated
engine recording a moving 1440p window at 120 FPS, saves 10 s apart, and a
second process streaming 20 MB/s of game assets from the same drive with the
file cache bypassed:

| Clip length, mode | Save times | Copied inside save | Asset reads > 50 ms (worst) |
|---|---|---|---|
| 60 s, overlap | 197 / 5242 / 4871 ms | up to 152 MB | 26 (380 ms) |
| 60 s, overlap, 16x backfill | 158 / 182 / 218 ms | KB | 4 (57 ms) |
| 120 s, overlap | 202 / 15024 / 13810 ms | hundreds of MB | 82 (625 ms) |
| 120 s, overlap, uncapped backfill | 184 / 7685 / 9810 ms | 241-340 MB | 52 (179 ms) |
| 120 s, consume (no overlap) | 167 / 110 / 133 ms | 16-45 KB | 5 (68 ms) |

The drive delivered about 30 MB/s for these copies while also recording and
serving the reader, so no backfill speed hides 400+ MB per save. During a long
synchronous copy the engine answers no request, so the app froze and
diagnostics timed out (seen on a user's PC saving to a hard drive). A window
that does not touch the disk showed no frame-time spikes in any mode, and
recording dropped no frames, so the cost was save latency and disk contention
for anything on the same drive, not system-wide hitches.

## Decision

- Save in place always consumes the saved window: each save starts where the
  previous successful save ended, keeping only the decoder preroll and AAC
  lookbehind it needs, which is copied back at once.
- `saveInPlaceOverlap` is removed from settings, the host contract, the engine
  `configure` command and the UI. Profiles that still carry it are normalized
  without it.
- The adaptive overlap backfill pace is removed; the preroll copy and the
  save-time copy keep their fixed multipliers.
- With Save in place off, the legacy RAM path is unchanged and still overlaps.

## Consequences

- Two saves close together produce one full clip and one short clip with only
  the new footage, instead of two largely duplicate files.
- Saves stay around 100-200 ms on hard drives and SSDs regardless of how soon
  they follow each other, and the background copy that competed with games on
  the same drive is gone.
- Reintroducing overlap on NTFS needs a design that does not copy the shared
  footage on the request thread; block cloning (ReFS / Dev Drive) is the only
  zero-copy option and is not the common case.
