# ADR 0030: Thumbnails as cached files, and commands that queue

Date: 2026-11-01

Status: implemented.

## Context

The window process sends commands to the resident host over a pipe. The host
admitted six at once (four for background work, two reserved for playback)
and answered any command that found the slots full with "busy" at once. A
window opening sends a burst: a thumbnail for every visible clip, process
icons, diagnostics, the friends list. Thumbnails were made by FFmpeg inside
the command, one at a time behind a lock, kept only in memory, and returned
as base64 through the pipe, so every launch re-ran FFmpeg for every card and
held background slots while it did. Whatever lost the race failed: the
friends list stayed on its placeholders until a sharing event arrived, and
the Friends page showed "Clipture is busy".

Measured with the real FFmpeg on 40 clips (1440p60, 3 audio tracks): one at a
time, 9.8 s on every launch, about 364 ms per thumbnail on a real clip.

## Decision

- Command admission waits instead of rejecting: a command that finds the
  slots full waits up to 15 s for one, off the pipe reader. Only a host busy
  past that, or more than 128 waiting commands, answers busy. Background
  commands take their background slot before a total slot, so waiting work
  never holds the playback reserve.
- `clip_thumbnail_url` returns a URL at once (no FFmpeg):
  `<media endpoint>/v1/thumbnail/<token>/image`, a random token mapped to the
  authorized clip and its version (size and modification time). The image
  loads through the media protocol with its own admission budget (four,
  queued up to 30 s), never the player's range-read slots.
- Thumbnails are JPEG files in `thumbnails/` in the data folder, named
  `<clip key>-<version key>` (BLAKE3 of the path and of the version; no path
  in the name), about 12 KiB each, at most 500 (oldest first), made by an
  FFmpeg executor with two slots of its own. One clip keeps one file: a new
  version replaces it and deleting the clip deletes it. Concurrent requests
  for one clip run FFmpeg once. Saved clips get theirs right after the save
  (`SavedClipProcessor::after_saved`), off the save path.
- The window treats thumbnails as images: a skeleton until the image decodes,
  then a fade; lazy outside the view; the player's poster and its picture
  while playback is prepared. The renderer's thumbnail queue and busy retries
  are removed.

## Consequences

- 40 clips: the first launch makes all thumbnails in 4.8 s (first one at
  0.25 s); every later launch reads them in 27 ms. URLs for all 40 take 7 ms.
- A window opening no longer turns commands away; the friends list loads
  with the library.
- Up to about 6 MiB of thumbnails on disk. Clips deleted outside Clipture
  keep theirs until the cap removes them.
