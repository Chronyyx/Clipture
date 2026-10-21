import { Check, LoaderCircle, RotateCcw, Volume2, VolumeX, X } from 'lucide-react';
import { useEffect, useRef, useState, type CSSProperties } from 'react';
import { dbToGain, gainToDb, MAXIMUM_TRACK_DB, MINIMUM_TRACK_DB, perceivedLoudness, type ClipMix } from './useClipMix';

/** The fader's position, in dB; silence sits at the bottom. */
const faderDb = (gain: number) => Math.max(MINIMUM_TRACK_DB, gainToDb(gain));
/** The level as heard: 100% is the original loudness. */
const loudness = (gain: number) => `${Math.round(perceivedLoudness(gain) * 100)}%`;
/** Where a fader sits along its slot, 0 to 1. */
const travel = (gain: number) => (faderDb(gain) - MINIMUM_TRACK_DB) / (MAXIMUM_TRACK_DB - MINIMUM_TRACK_DB);
/** Meters read peak level from -48 dBFS to full scale. */
const METER_FLOOR_DB = -48;
/** Peak hold falls back at this many dB per second, like a console meter. */
const METER_RELEASE_DB = 24;

/** A level and mute per audio track, under the player. Changes are heard
 * at once; "Save to file" re-encodes the changed tracks for good. `peaks`
 * reads each track's live level when playback holds the tracks apart. */
export function AudioMixer({ mix, peaks }: { mix: ClipMix; peaks?: () => number[] }) {
  const [confirming, setConfirming] = useState(false);
  const rows = useRef<(HTMLDivElement | null)[]>([]);
  useEffect(() => { if (mix.changed === 0) setConfirming(false); }, [mix.changed]);
  useMeters(rows, peaks);
  const tracksText = mix.changed === 1 ? '1 track' : `${mix.changed} tracks`;
  const status = mix.busy
    ? `Re-encoding ${tracksText}…`
    : confirming
      ? `Re-encode ${tracksText} at these levels for good?`
      : mix.changed > 0
        ? `${tracksText} changed`
        : 'Changes play as you move them';

  return (
    <div className='edit-deck audio-mixer' aria-label='Audio mixer' aria-busy={mix.busy} data-saving={mix.busy || undefined}>
      <div className='deck-header'>
        <h2 className='deck-title'>Audio mixer</h2>
        <button className='deck-key deck-close' type='button' disabled={mix.busy} title='Close the mixer' aria-label='Close the mixer' onClick={mix.close}>
          <X size={16} aria-hidden='true' />
        </button>
      </div>
      <div className='mixer-tracks deck-body'>
        {mix.tracks.map((track, position) => (
          <div
            key={track.index}
            ref={(row) => { rows.current[position] = row; }}
            className={`mixer-track${track.muted ? ' muted' : ''}`}
            style={{ '--fader': travel(track.muted ? 0 : track.gain) } as CSSProperties}
          >
            <button
              className={`deck-key mixer-mute${track.muted ? ' active' : ''}`}
              type='button'
              disabled={mix.busy}
              title={track.muted ? `Unmute ${track.label}` : `Mute ${track.label}`}
              aria-label={`Mute ${track.label}`}
              aria-pressed={track.muted}
              onClick={() => mix.toggleMute(track.index)}
            >
              {track.muted ? <VolumeX size={15} aria-hidden='true' /> : <Volume2 size={15} aria-hidden='true' />}
            </button>
            <span className='mixer-name' title={track.label}>{track.label}</span>
            <div className='mixer-strip'>
              <input
                className='mixer-fader'
                type='range'
                min={MINIMUM_TRACK_DB}
                max={MAXIMUM_TRACK_DB}
                step={0.5}
                value={track.muted ? MINIMUM_TRACK_DB : faderDb(track.gain)}
                disabled={mix.busy}
                aria-label={`${track.label} level`}
                aria-valuetext={track.muted ? 'Muted' : `${loudness(track.gain)} as loud`}
                onChange={(event) => mix.setGain(track.index, dbToGain(Number(event.target.value)))}
                // The player seeks on arrow keys; here they move the level.
                onKeyDown={(event) => { if (event.key.startsWith('Arrow')) event.stopPropagation(); }}
                onDoubleClick={() => mix.setGain(track.index, 1)}
              />
              <span className='mixer-meter' aria-hidden='true' />
            </div>
            <output className='deck-readout mixer-value' title={track.muted ? undefined : `${Math.round(gainToDb(track.gain) * 2) / 2} dB`}>
              {track.muted ? 'Muted' : loudness(track.gain)}
            </output>
          </div>
        ))}
        <div className='mixer-scale' aria-hidden='true'><span>Quieter</span><span>Original</span><span>Louder</span></div>
      </div>
      <div className='deck-footer'>
        <span className={`deck-status${confirming ? ' warning' : ''}`} role='status'>{status}</span>
        {mix.error && <span className='deck-error' role='alert'>{mix.error}</span>}
        <span className='deck-spacer' />
        {confirming || mix.busy ? (
          <>
            <button className='deck-key' type='button' disabled={mix.busy} onClick={() => setConfirming(false)}>Keep editing</button>
            <button className='deck-key deck-primary' type='button' disabled={mix.busy} onClick={() => void mix.apply()}>
              {mix.busy ? <LoaderCircle className='deck-spinner' size={15} aria-hidden='true' /> : <Check size={15} aria-hidden='true' />}
              {mix.busy ? 'Saving…' : 'Save to file'}
            </button>
          </>
        ) : (
          <>
            <button className='deck-key' type='button' disabled={mix.changed === 0} onClick={mix.reset}>
              <RotateCcw size={15} aria-hidden='true' /> Reset
            </button>
            <button className='deck-key deck-primary' type='button' disabled={mix.changed === 0} onClick={() => setConfirming(true)}>
              <Check size={15} aria-hidden='true' /> Save to file
            </button>
          </>
        )}
      </div>
    </div>
  );
}

/** Drives each row's meter from `peaks` once a frame, without re-rendering:
 * instant attack, a steady fall, and a clip flag at full scale. */
function useMeters(rows: { current: (HTMLDivElement | null)[] }, peaks?: () => number[]) {
  useEffect(() => {
    if (!peaks) return;
    const held: number[] = [];
    // A clip stays lit for a moment so it can be seen.
    const clippedUntil: number[] = [];
    let last = performance.now();
    let frame = 0;
    const tick = (now: number) => {
      const fall = ((now - last) / 1000) * METER_RELEASE_DB;
      last = now;
      const levels = peaks();
      rows.current.forEach((row, track) => {
        if (!row) return;
        const peak = levels[track] ?? 0;
        const db = peak > 0 ? 20 * Math.log10(peak) : METER_FLOOR_DB;
        held[track] = Math.max(db, (held[track] ?? METER_FLOOR_DB) - fall);
        const level = Math.min(1, Math.max(0, (held[track] - METER_FLOOR_DB) / -METER_FLOOR_DB));
        row.style.setProperty('--meter', level.toFixed(3));
        if (peak >= 0.999) clippedUntil[track] = now + 1200;
        row.toggleAttribute('data-clipping', now < (clippedUntil[track] ?? 0));
      });
      frame = requestAnimationFrame(tick);
    };
    frame = requestAnimationFrame(tick);
    return () => {
      cancelAnimationFrame(frame);
      for (const row of rows.current) {
        row?.style.removeProperty('--meter');
        row?.removeAttribute('data-clipping');
      }
    };
  }, [peaks, rows]);
}
