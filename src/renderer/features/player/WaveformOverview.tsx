import { useRef } from 'react';
import type { ClipWaveform } from './clipWaveform';
import { alpha, prepare, readPalette, useCanvasDrawing, wavePath } from './waveformCanvas';

/** The whole clip's audio behind the trim strip, laid over the same 0 to
 * `duration` as the strip's handles, with a box marking what the detail
 * view shows (`view`, in seconds). Pointer input goes to the strip. */
export function WaveformOverview({ waveform, duration, view }: {
  waveform: ClipWaveform;
  duration: number;
  view: { start: number; seconds: number };
}) {
  const canvas = useRef<HTMLCanvasElement>(null);
  useCanvasDrawing(canvas, (width, height) => {
    const element = canvas.current;
    if (!element || duration <= 0) return;
    const context = prepare(element, width, height);
    if (!context) return;
    const colors = readPalette(element);
    context.fillStyle = colors.wave;
    context.fill(wavePath(waveform, 0, duration, width, height));
    const left = (view.start / duration) * width;
    const right = (Math.min(duration, view.start + view.seconds) / duration) * width;
    context.fillStyle = alpha(colors.region, 0.22);
    context.strokeStyle = alpha(colors.region, 0.8);
    context.lineWidth = 1;
    context.beginPath();
    context.roundRect(left + 0.5, 3.5, Math.max(2, right - left - 1), height - 7, 2);
    context.fill();
    context.stroke();
  }, [waveform, duration, view.start, view.seconds]);
  return <canvas ref={canvas} className='trim-wave ready' aria-hidden='true' />;
}
