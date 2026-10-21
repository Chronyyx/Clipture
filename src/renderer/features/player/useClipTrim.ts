import { useEffect, useState, type RefObject } from 'react';
import type { ClipRecord } from '../../../shared/types';
import { clipture, userMessage } from '../../platform';
import { formatDuration } from '../../shared/clips/clipMetadata';
import { seekCoalesced } from './seekCoalesced';
import { useTrimLoop, type TrimRange } from './TrimEditor';

/** Only Clipture's own single-file clips are rewritten; imported videos
 * belong to the user and are never changed. */
export function canTrim(clip: ClipRecord) {
  return clip.librarySource !== 'imported' && !clip.id.startsWith('imported:') && !clip.segmentFiles?.length;
}

function formatSize(bytes: number) {
  return bytes >= 1e9 ? `${(bytes / 1e9).toFixed(1)} GB` : `${Math.max(1, Math.round(bytes / 1e6))} MB`;
}

/** The player's trim mode: the range being kept, and applying it. */
export function useClipTrim(clip: ClipRecord, videoRef: RefObject<HTMLVideoElement | null>, duration: number, onTrimmed: () => void) {
  const [range, setRange] = useState<TrimRange | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');

  useEffect(() => { setRange(null); setError(''); }, [clip.id]);
  useEffect(() => {
    if (!notice) return;
    const timer = setTimeout(() => setNotice(''), 6000);
    return () => clearTimeout(timer);
  }, [notice]);
  useTrimLoop(videoRef, range);

  return {
    trimmable: canTrim(clip),
    range,
    busy,
    error,
    notice,
    toggle: () => {
      setError('');
      setRange((current) => (current ? null : { start: 0, end: duration }));
    },
    change: setRange,
    seek: (seconds: number) => {
      const video = videoRef.current;
      if (video) seekCoalesced(video, seconds);
    },
    apply: async () => {
      if (!range) return;
      setBusy(true);
      setError('');
      videoRef.current?.pause();
      try {
        const result = await clipture.trimClip(clip.id, range.start, range.end);
        setRange(null);
        setNotice(`Trimmed to ${formatDuration(result.durationSeconds)} · ${formatSize(Math.max(0, result.bytesBefore - result.bytesAfter))} freed`);
        onTrimmed();
      } catch (failure) {
        setError(userMessage(failure, 'The clip could not be trimmed.'));
      } finally {
        setBusy(false);
      }
    }
  };
}
