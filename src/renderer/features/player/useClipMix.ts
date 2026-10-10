import { useEffect, useMemo, useState } from 'react';
import type { ClipRecord } from '../../../shared/types';
import { clipture, userMessage } from '../../platform';
import { displayAudioTrackName } from '../../shared/clips/clipMetadata';
import { canTrim } from './useClipTrim';

/** Fader range in decibels, even around 0 dB (unchanged) so that sits in the
 * middle; the bottom of the fader is silence. Loudness is heard in
 * proportion to decibels, so equal steps sound equal. */
export const MINIMUM_TRACK_DB = -20;
export const MAXIMUM_TRACK_DB = 20;
/** The loudest a track can be set, as a gain (+20 dB, 10x). */
export const MAXIMUM_TRACK_GAIN = 10 ** (MAXIMUM_TRACK_DB / 20);

export const gainToDb = (gain: number) => (gain <= 0 ? -Infinity : 20 * Math.log10(gain));
export const dbToGain = (db: number) => (db <= MINIMUM_TRACK_DB ? 0 : Math.min(MAXIMUM_TRACK_GAIN, 10 ** (db / 20)));
/** How loud a level sounds against the original: +10 dB sounds about twice
 * as loud, so 0 dB is 100%, +10 dB 200% and +20 dB 400%. */
export const perceivedLoudness = (gain: number) => (gain <= 0 ? 0 : 2 ** (gainToDb(gain) / 10));
/** Closer to 100% than this counts as unchanged. */
const UNCHANGED = 0.005;

interface Level {
  gain: number;
  muted: boolean;
}

export interface MixerTrack extends Level {
  /** Position among the clip's audio tracks. */
  index: number;
  label: string;
}

const neutral: Level = { gain: 1, muted: false };
const effective = (level: Level) => (level.muted ? 0 : level.gain);

/** The player's mixer: a level per audio track, heard live while open, and
 * baking them into the clip's file. `trackIndexes` are the tracks playback
 * mixes, in the order its per-track audio holds them. */
export function useClipMix(clip: ClipRecord, trackIndexes: number[], onMixed: () => void) {
  const [open, setOpen] = useState(false);
  const [levels, setLevels] = useState<Record<number, Level>>({});
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');

  useEffect(() => { setOpen(false); setLevels({}); setError(''); }, [clip.id]);
  useEffect(() => {
    if (!notice) return;
    const timer = setTimeout(() => setNotice(''), 6000);
    return () => clearTimeout(timer);
  }, [notice]);

  const tracks: MixerTrack[] = useMemo(
    () => trackIndexes.map((index) => ({
      index,
      label: displayAudioTrackName(clip.audioTracks[index] ?? `Track ${index + 1}`),
      ...(levels[index] ?? neutral)
    })),
    [clip.audioTracks, levels, trackIndexes]
  );
  const changed = tracks.filter((track) => Math.abs(effective(track) - 1) > UNCHANGED).length;
  const update = (index: number, change: Partial<Level>) =>
    setLevels((current) => ({ ...current, [index]: { ...(current[index] ?? neutral), ...change } }));

  return {
    mixable: canTrim(clip) && trackIndexes.length > 0,
    open,
    tracks,
    changed,
    busy,
    error,
    notice,
    /** Per-track gains for live playback while the mixer is open. */
    previewGains: open ? tracks.map(effective) : undefined,
    toggle: () => {
      setError('');
      setOpen((current) => !current);
    },
    close: () => setOpen(false),
    setGain: (index: number, gain: number) => update(index, { gain: Math.min(MAXIMUM_TRACK_GAIN, Math.max(0, gain)), muted: false }),
    toggleMute: (index: number) => update(index, { muted: !(levels[index] ?? neutral).muted }),
    reset: () => setLevels({}),
    apply: async () => {
      setBusy(true);
      setError('');
      try {
        // Every audio track of the file, by position; untouched ones stay at 1.
        const gains = clip.audioTracks.map((_, index) => effective(levels[index] ?? neutral));
        const result = await clipture.mixClipAudio(clip.id, gains);
        setLevels({});
        setOpen(false);
        setNotice(result.tracksChanged === 1 ? 'New level saved to 1 track' : `New levels saved to ${result.tracksChanged} tracks`);
        onMixed();
      } catch (failure) {
        setError(userMessage(failure, 'The new levels could not be saved.'));
      } finally {
        setBusy(false);
      }
    }
  };
}

export type ClipMix = ReturnType<typeof useClipMix>;
