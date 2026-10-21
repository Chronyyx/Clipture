import { LoaderCircle, Minus, Plus, Scissors, X } from 'lucide-react';
import { useEffect, useRef, useState, type CSSProperties, type PointerEvent as ReactPointerEvent, type RefObject } from 'react';
import { formatDuration } from '../../shared/clips/clipMetadata';
import type { ClipWaveform } from './clipWaveform';
import { followPlayback } from './followPlayback';
import { WaveformOverview } from './WaveformOverview';
import { WaveformZoom, ZOOM_STEPS } from './WaveformZoom';

export interface TrimRange {
  start: number;
  end: number;
}

/** Shortest part worth keeping, in seconds. */
const MINIMUM_KEPT = 0.5;

/** m:ss.t, to the nearest tenth. */
const precise = (seconds: number) => {
  const tenths = Math.round(Math.max(0, seconds) * 10);
  return `${formatDuration(Math.floor(tenths / 10))}.${tenths % 10}`;
};

/** The trim bar shown under the player while editing. Dragging a handle
 * moves the playhead with it, so the frame you land on is the cut. */
export function TrimEditor({
  video,
  duration,
  range,
  waveform,
  busy,
  error,
  onChange,
  onSeek,
  onCancel,
  onApply
}: {
  /** The player's video: the waveform views seek it and follow it. */
  video: RefObject<HTMLVideoElement | null>;
  duration: number;
  range: TrimRange;
  /** The clip's audio, drawn behind the strip and in the detail view. */
  waveform?: ClipWaveform;
  busy: boolean;
  error: string;
  onChange: (range: TrimRange) => void;
  onSeek: (seconds: number) => void;
  onCancel: () => void;
  onApply: () => void;
}) {
  const track = useRef<HTMLDivElement>(null);
  const [zoom, setZoom] = useState(2);
  const seconds = Math.min(ZOOM_STEPS[zoom], Math.max(0.5, duration));
  // The detail view's left edge, in seconds.
  const [viewStart, setViewStart] = useState(0);
  const clampView = (start: number) => Math.max(0, Math.min(Math.max(0, duration - seconds), start));
  // A new zoom opens around the playhead.
  useEffect(() => {
    setViewStart(clampView((video.current?.currentTime ?? 0) - seconds / 2));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [seconds]);
  // Pointer and key events can outrun renders; they work from the latest range.
  const latest = useRef(range);
  latest.current = range;
  const dragging = useRef<'start' | 'end'>(undefined);
  const [confirming, setConfirming] = useState(false);
  const unchanged = range.start <= 0.05 && range.end >= duration - 0.05;
  useEffect(() => { if (unchanged) setConfirming(false); }, [unchanged]);

  const percent = (seconds: number) => (duration > 0 ? (seconds / duration) * 100 : 0);
  // The strip's playhead follows the video every frame, not at React's pace.
  useEffect(() => {
    const element = video.current;
    const strip = track.current;
    if (!element || !strip) return;
    const place = () => strip.style.setProperty('--trim-playhead', `${percent(element.currentTime)}%`);
    place();
    return followPlayback(element, place);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [video, duration]);
  // The frame on screen, not the last time React heard about.
  const playhead = () => video.current?.currentTime ?? 0;
  const secondsAt = (event: ReactPointerEvent) => {
    const box = track.current?.getBoundingClientRect();
    if (!box || box.width === 0) return 0;
    return Math.min(duration, Math.max(0, ((event.clientX - box.left) / box.width) * duration));
  };
  const move = (edge: 'start' | 'end', seconds: number) => {
    const current = latest.current;
    const next = edge === 'start'
      ? { start: Math.min(seconds, current.end - MINIMUM_KEPT), end: current.end }
      : { start: current.start, end: Math.max(seconds, current.start + MINIMUM_KEPT) };
    next.start = Math.max(0, next.start);
    next.end = Math.min(duration, next.end);
    latest.current = next;
    onChange(next);
    onSeek(edge === 'start' ? next.start : next.end);
  };

  return (
    <div className='edit-deck trim-editor' aria-label='Trim clip' aria-busy={busy} data-saving={busy || undefined}>
      <div className='deck-header'>
        <h2 className='deck-title'>Trim</h2>
        <button className='deck-key deck-close' type='button' disabled={busy} title='Close the trim editor' aria-label='Close the trim editor' onClick={onCancel}>
          <X size={16} aria-hidden='true' />
        </button>
      </div>
      <div className='deck-body trim-body'>
      <div className='trim-row'>
        <span className='trim-label'>Keep</span>
        <div
          ref={track}
          className='trim-track'
          style={{
            '--trim-start': `${percent(range.start)}%`,
            '--trim-end': `${percent(range.end)}%`
          } as CSSProperties}
          onPointerDown={(event) => {
            if (busy) return;
            const seconds = secondsAt(event);
            // Grab whichever handle is nearer.
            const edge = Math.abs(seconds - range.start) <= Math.abs(seconds - range.end) ? 'start' : 'end';
            try { event.currentTarget.setPointerCapture(event.pointerId); } catch { /* pointer already gone */ }
            dragging.current = edge;
            move(edge, seconds);
          }}
          onPointerMove={(event) => { if (dragging.current) move(dragging.current, secondsAt(event)); }}
          onPointerUp={() => { dragging.current = undefined; }}
          onPointerCancel={() => { dragging.current = undefined; }}
        >
          {waveform && <WaveformOverview waveform={waveform} duration={duration} view={{ start: viewStart, seconds }} />}
          <span className='trim-cut trim-cut-before' aria-hidden='true' />
          <span className='trim-kept' aria-hidden='true' />
          <span className='trim-cut trim-cut-after' aria-hidden='true' />
          <span className='trim-playhead' aria-hidden='true' />
          <TrimHandle edge='start' at={() => latest.current.start} duration={duration} disabled={busy} onMove={(seconds) => move('start', seconds)} />
          <TrimHandle edge='end' at={() => latest.current.end} duration={duration} disabled={busy} onMove={(seconds) => move('end', seconds)} />
        </div>
        <output className='deck-readout trim-length' title='Length after trimming'>{precise(range.end - range.start)}</output>
      </div>
      {waveform && (
        <div className='trim-detail'>
          <div className='trim-detail-head'>
            <span className='trim-detail-label'>Detail</span>
            <span className='deck-readout'>{seconds} s</span>
            <button className='deck-key trim-zoom' type='button' title='Zoom in' aria-label='Zoom in' disabled={zoom === 0} onClick={() => setZoom(zoom - 1)}>
              <Plus size={15} aria-hidden='true' />
            </button>
            <button className='deck-key trim-zoom' type='button' title='Zoom out' aria-label='Zoom out' disabled={zoom === ZOOM_STEPS.length - 1} onClick={() => setZoom(zoom + 1)}>
              <Minus size={15} aria-hidden='true' />
            </button>
          </div>
          <WaveformZoom
            waveform={waveform}
            duration={duration}
            seconds={seconds}
            viewStart={viewStart}
            onViewStart={(start) => setViewStart(clampView(start))}
            range={range}
            onRange={(edge, time) => { if (!busy) move(edge, time); }}
            video={video}
            onSeek={onSeek}
          />
        </div>
      )}
      <div className='trim-row trim-marks'>
        <button className='deck-key' type='button' disabled={busy} onClick={() => move('start', playhead())}>Start here</button>
        <button className='deck-key' type='button' disabled={busy} onClick={() => move('end', playhead())}>End here</button>
        <span className='deck-readout trim-span'>{precise(range.start)} – {precise(range.end)} · keeps {(range.end - range.start).toFixed(1)} s</span>
      </div>
      </div>
      <div className='deck-footer'>
        <span className={`deck-status${confirming || busy ? ' warning' : ''}`} role='status'>
          {busy
            ? 'Deleting the rest of the clip…'
            : confirming
              ? `Keep ${precise(range.start)} – ${precise(range.end)} and delete the rest from the file for good?`
              : 'Drag the handles or set them at the playhead'}
        </span>
        {error && <span className='deck-error' role='alert'>{error}</span>}
        <span className='deck-spacer' />
        {confirming || busy ? (
          <>
            <button className='deck-key' type='button' disabled={busy} onClick={() => setConfirming(false)}>Keep editing</button>
            <button className='deck-key deck-primary' type='button' disabled={busy} onClick={onApply}>
              {busy ? <LoaderCircle className='deck-spinner' size={15} aria-hidden='true' /> : <Scissors size={15} aria-hidden='true' />}
              {busy ? 'Trimming…' : 'Delete the rest'}
            </button>
          </>
        ) : (
          <>
            <button className='deck-key deck-primary' type='button' disabled={unchanged} onClick={() => setConfirming(true)}>
              <Scissors size={15} aria-hidden='true' /> Trim clip
            </button>
          </>
        )}
      </div>
    </div>
  );
}

/** A handle you can also nudge with the arrow keys (a tenth of a second,
 * a second with Shift). */
function TrimHandle({ edge, at: position, duration, disabled, onMove }: {
  edge: 'start' | 'end';
  at: () => number;
  duration: number;
  disabled: boolean;
  onMove: (seconds: number) => void;
}) {
  const at = position();
  return (
    <span
      className={`trim-handle trim-handle-${edge}`}
      role='slider'
      tabIndex={disabled ? -1 : 0}
      aria-label={edge === 'start' ? 'Trim start' : 'Trim end'}
      aria-valuemin={0}
      aria-valuemax={Math.round(duration * 10) / 10}
      aria-valuenow={Math.round(at * 10) / 10}
      aria-valuetext={precise(at)}
      onKeyDown={(event) => {
        const step = event.shiftKey ? 1 : 0.1;
        if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
        // The player seeks on arrow keys too; here they move the handle.
        event.preventDefault();
        event.stopPropagation();
        onMove(position() + (event.key === 'ArrowLeft' ? -step : step));
      }}
    />
  );
}

/** While editing, playback loops inside the kept part. */
export function useTrimLoop(videoRef: RefObject<HTMLVideoElement | null>, range: TrimRange | null) {
  useEffect(() => {
    const video = videoRef.current;
    if (!range || !video) return;
    // Checked every frame while playing only; a paused video needs nothing.
    return followPlayback(video, () => {
      if (!video.paused && (video.currentTime >= range.end || video.currentTime < range.start - 0.25)) {
        video.currentTime = range.start;
      }
    });
  }, [videoRef, range?.start, range?.end]);
}
