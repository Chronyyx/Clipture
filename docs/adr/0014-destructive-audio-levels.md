# ADR 0014: Destructive per-track audio levels

Date: 2026-10-03

Status: accepted; the player's audio mixer for Clipture's own clips.

## Context

Clips keep each audio source as its own AAC track (system audio, microphone,
each separately captured app). Balancing them should change the clip itself,
so every player hears the new balance, not just Clipture: other players and
uploads usually play one track, and Clipture's player mixes all of them.
That means changing the audio data of each adjusted track, not storing a
gain beside it. As with trimming (ADR 0013), the video must not be
re-encoded or copied, and a crash must never leave a broken clip.

## Decision

`media/mix` re-encodes only the tracks whose level changed and writes them
back into the clip in place:

1. **Encode.** FFmpeg decodes each changed track from its first media sample
   (`-ignore_editlist 1`), applies `volume` (0 silences the track), and
   encodes AAC at 90% of that track's own bitrate into a small audio-only MP4
   in the journal folder. Decoding without edit lists makes the new media
   timeline the old one shifted by the encoder's priming. The track's edit
   list therefore keeps its empty edits and durations, and its media edit
   starts later by that priming. Video and unchanged tracks are never
   decoded.
2. **Layout.** `plan.rs` fills the track's old packet slots with its new
   packets in time order: a slot takes as many as fit, the rest of it is
   zeroed, and the next packet moves on to the next slot. The clip therefore
   keeps its interleaving (in-order recordings stay in order), and none of
   the old audio is left behind. Packets no slot can take (a louder track
   can outgrow its old bitrate) go after the media, which grows to hold
   them, followed by the new index. The changed tracks get new sample
   tables: the encoder's description and timing, one packet per chunk,
   64-bit offsets.
3. **Crash safety.** `journal.rs` writes every new byte (slots, overflow,
   media header, index) with its offset and the final length, flushes it,
   then writes a BLAKE3-checked header as the commit point. Applying never
   reads the clip, so applying the journal again from the start is always
   safe. An uncommitted journal is discarded with the clip untouched. A
   committed one is finished at the next startup
   (`MediaService::finish_interrupted_mixes`). FFmpeg reads the clip before
   it is opened exclusively, and the index is compared afterwards: a clip
   that changed in between is refused.
4. **Exclusive access** is as for trimming: no sharing, a short wait for the
   recorder, then "busy".

The extra disk used is the journal: about the size of the changed tracks'
audio. Shares of a changed clip end.

Live preview needs no file change. With the mixer open, playback asks for
audio chunks with `tracks=1`: one stereo pair per track side by side
(FFmpeg `amerge`). Web Audio splits the pairs, gives each its own gain, and
sums them at 1/N, as loud as the mixed chunks. Levels are heard as they
move.

## Consequences

- A two-minute clip with two changed tracks is remixed in about 1.3 s, most
  of it encoding. With the microphone at 50% and Discord muted, the video
  packets were byte-identical, the microphone matched the original at half
  level with zero samples of lag (−40 dB residual, the AAC generation loss)
  and Discord decoded as silence.
- Every change is another AAC generation for the changed tracks. Unchanged
  tracks (within 0.5% of 100%) are left alone.
- The mixer's faders work in decibels, as loudness is heard: −20 dB to
  +20 dB with 0 dB in the middle and silence at the bottom, labelled as
  perceived loudness (+10 dB sounds twice as loud: 25% to 400%). The host
  takes the matching linear gain, at most 10. A boosted loud track can clip;
  nothing limits it.
- There is no undo. The player asks for confirmation before saving.
