import { useEffect, useRef, type PointerEvent as ReactPointerEvent, type RefObject } from 'react';
import type { ClipWaveform } from './clipWaveform';
import { followPlayback } from './followPlayback';
import type { TrimRange } from './TrimEditor';
import { alpha, prepare, readPalette, useCanvasDrawing, wavePath, type WavePalette } from './waveformCanvas';

/** How much of the clip the detail view shows, in seconds, from closest. */
export const ZOOM_STEPS = [2, 4, 8, 16, 32] as const;
/** How close (in pixels) a press must be to grab an edge of the kept part. */
const GRAB = 8;
/** A press that moves less than this is a click (seek), not a pan. */
const CLICK_SLOP = 3;
/** While playing, the view turns a page when the playhead nears its edge. */
const PAGE_AT = 0.92;
const PAGE_LEAD = 0.08;

interface Drag {
  kind: 'start' | 'end' | 'pan';
  x: number;
  viewStart: number;
  moved: boolean;
}

/** The detail waveform: `seconds` of the clip from `viewStart`, with a time
 * axis, the kept part as a region whose edges drag, and the playhead, which
 * moves every frame while the video plays. Click to seek; drag the waveform
 * or use the wheel to scroll. */
export function WaveformZoom({ waveform, duration, seconds, viewStart, onViewStart, range, onRange, video, onSeek }: {
  waveform: ClipWaveform;
  duration: number;
  seconds: number;
  viewStart: number;
  onViewStart: (start: number) => void;
  range: TrimRange;
  onRange: (edge: 'start' | 'end', seconds: number) => void;
  video: RefObject<HTMLVideoElement | null>;
  onSeek: (seconds: number) => void;
}) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const drag = useRef<Drag>(undefined);
  const latest = useRef({ seconds, viewStart, range, duration, onViewStart });
  latest.current = { seconds, viewStart, range, duration, onViewStart };
  const clampStart = (start: number) => Math.max(0, Math.min(Math.max(0, duration - seconds), start));

  // Everything but the playhead, drawn once per change and copied each frame.
  const still = useRef<{ layer: HTMLCanvasElement; colors: WavePalette }>(undefined);

  const { size, redraw } = useCanvasDrawing(canvas, (width, height, changed) => {
    const element = canvas.current;
    if (!element) return;
    const scale = window.devicePixelRatio || 1;
    let current = still.current;
    if (changed || !current || current.layer.width !== Math.max(1, Math.round(width * scale))) {
      const layer = current?.layer ?? document.createElement('canvas');
      const layerContext = prepare(layer, width, height);
      if (!layerContext) return;
      current = { layer, colors: readPalette(element) };
      drawStill(layerContext, current.colors, waveform, viewStart, seconds, range, width, height);
      still.current = current;
    }
    const context = prepare(element, width, height);
    if (!context) return;
    context.drawImage(current.layer, 0, 0, width, height);
    const playhead = video.current?.currentTime ?? 0;
    context.fillStyle = current.colors.playhead;
    context.fillRect(Math.round(((playhead - viewStart) / seconds) * width) - 1, 0, 2, height);
  }, [waveform, seconds, viewStart, range.start, range.end]);

  // While playing: redraw every frame for a smooth playhead, and turn the
  // page when it reaches the edge.
  useEffect(() => {
    const element = video.current;
    if (!element) return;
    return followPlayback(element, () => {
      const now = latest.current;
      const time = element.currentTime;
      if (!drag.current && (time > now.viewStart + now.seconds * PAGE_AT || time < now.viewStart)) {
        const next = Math.max(0, Math.min(Math.max(0, now.duration - now.seconds), time - now.seconds * PAGE_LEAD));
        if (Math.abs(next - now.viewStart) > 0.001) now.onViewStart(next);
      }
      redraw();
    });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [video]);

  // The wheel scrolls through the clip (non-passive, so the page stays put).
  useEffect(() => {
    const element = canvas.current;
    if (!element) return;
    const wheel = (event: WheelEvent) => {
      const now = latest.current;
      const width = size.current.width || 1;
      const delta = Math.abs(event.deltaX) > Math.abs(event.deltaY) ? event.deltaX : event.deltaY;
      if (delta === 0) return;
      event.preventDefault();
      const next = Math.max(0, Math.min(Math.max(0, now.duration - now.seconds), now.viewStart + (delta / width) * now.seconds));
      now.onViewStart(next);
    };
    element.addEventListener('wheel', wheel, { passive: false });
    return () => element.removeEventListener('wheel', wheel);
  }, [size]);

  const timeAt = (event: ReactPointerEvent) => {
    const box = canvas.current?.getBoundingClientRect();
    if (!box || box.width === 0) return viewStart;
    return viewStart + ((event.clientX - box.left) / box.width) * seconds;
  };
  const edgeAt = (event: ReactPointerEvent): Drag['kind'] => {
    const box = canvas.current?.getBoundingClientRect();
    if (!box) return 'pan';
    const pointer = event.clientX - box.left;
    const at = (time: number) => ((time - viewStart) / seconds) * box.width;
    const toStart = Math.abs(pointer - at(range.start));
    const toEnd = Math.abs(pointer - at(range.end));
    if (Math.min(toStart, toEnd) > GRAB) return 'pan';
    return toStart <= toEnd ? 'start' : 'end';
  };

  return (
    <canvas
      ref={canvas}
      className='trim-zoomview ready'
      aria-label='Waveform detail. Click to move the playhead, drag to scroll, drag the kept part’s edges to trim.'
      onPointerDown={(event) => {
        try { event.currentTarget.setPointerCapture(event.pointerId); } catch { /* pointer already gone */ }
        drag.current = { kind: edgeAt(event), x: event.clientX, viewStart, moved: false };
      }}
      onPointerMove={(event) => {
        const current = drag.current;
        if (!current) {
          event.currentTarget.style.cursor = edgeAt(event) === 'pan' ? 'pointer' : 'ew-resize';
          return;
        }
        const dx = event.clientX - current.x;
        if (Math.abs(dx) > CLICK_SLOP) current.moved = true;
        if (current.kind === 'pan') {
          if (current.moved) onViewStart(clampStart(current.viewStart - (dx / (size.current.width || 1)) * seconds));
        } else {
          onRange(current.kind, Math.max(0, Math.min(duration, timeAt(event))));
        }
      }}
      onPointerUp={(event) => {
        const current = drag.current;
        drag.current = undefined;
        if (current?.kind === 'pan' && !current.moved) onSeek(Math.max(0, Math.min(duration, timeAt(event))));
      }}
      onPointerCancel={() => { drag.current = undefined; }}
    />
  );
}

/** The axis, the waveform, and the kept part (tinted, outlined, its waveform
 * brighter) with its edges: all the detail view shows except the playhead. */
function drawStill(context: CanvasRenderingContext2D, colors: WavePalette, waveform: ClipWaveform, start: number, seconds: number, range: TrimRange, width: number, height: number) {
  const x = (time: number) => ((time - start) / seconds) * width;
  drawAxis(context, colors, start, seconds, width, height);
  const shape = wavePath(waveform, start, seconds, width, height);
  context.fillStyle = colors.wave;
  context.fill(shape);
  const left = Math.max(-2, x(range.start));
  const right = Math.min(width + 2, x(range.end));
  if (right > left) {
    context.fillStyle = alpha(colors.region, 0.14);
    context.fillRect(left, 0, right - left, height);
    context.save();
    context.beginPath();
    context.rect(left, 0, right - left, height);
    context.clip();
    context.fillStyle = colors.kept;
    context.fill(shape);
    context.restore();
    context.strokeStyle = colors.region;
    context.lineWidth = 2;
    context.strokeRect(left + 1, 1, right - left - 2, height - 2);
  }
  for (const time of [range.start, range.end]) drawMarker(context, colors.marker, x(time), height);
}

/** Time ticks along the top and bottom, labelled at the bottom left. */
function drawAxis(context: CanvasRenderingContext2D, colors: WavePalette, start: number, seconds: number, width: number, height: number) {
  const steps = [0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300];
  const step = steps.find((candidate) => (candidate / seconds) * width >= 64) ?? 600;
  context.fillStyle = colors.grid;
  context.font = `10px ${colors.font}`;
  context.textBaseline = 'bottom';
  for (let time = Math.ceil(start / step) * step; time <= start + seconds; time += step) {
    const x = Math.round(((time - start) / seconds) * width) + 0.5;
    context.fillStyle = colors.grid;
    context.fillRect(x - 0.5, 0, 1, 7);
    context.fillRect(x - 0.5, height - 7, 1, 7);
    const text = label(time, step);
    if (x + 4 + context.measureText(text).width > width - 2) continue;
    context.fillStyle = colors.label;
    context.fillText(text, x + 4, height - 4);
  }
}

/** mm:ss, with tenths when the ticks are closer than a second. */
function label(time: number, step: number) {
  const rounded = Math.round(time * 10) / 10;
  const minutes = Math.floor(rounded / 60);
  const whole = Math.floor(rounded % 60);
  const text = `${String(minutes).padStart(2, '0')}:${String(whole).padStart(2, '0')}`;
  return step < 1 ? `${text}.${Math.round((rounded % 1) * 10)}` : text;
}

/** An edge of the kept part: a line the full height with a grip midway. */
function drawMarker(context: CanvasRenderingContext2D, color: string, x: number, height: number) {
  context.fillStyle = color;
  context.fillRect(Math.round(x) - 1, 0, 2, height);
  context.beginPath();
  context.roundRect(Math.round(x) - 4, height / 2 - 12, 8, 24, 2);
  context.fill();
  context.fillStyle = 'rgba(255, 255, 255, 0.7)';
  context.fillRect(Math.round(x) - 1.5, height / 2 - 6, 1, 12);
  context.fillRect(Math.round(x) + 0.5, height / 2 - 6, 1, 12);
}
