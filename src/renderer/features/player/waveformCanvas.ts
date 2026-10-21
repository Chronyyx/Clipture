import { useEffect, useRef, type RefObject } from 'react';
import type { ClipWaveform } from './clipWaveform';

/** The trim editor's waveform colours, from the transport's theme tokens
 * (inherited by the canvas from the edit deck around it). */
export interface WavePalette {
  wave: string;
  kept: string;
  playhead: string;
  marker: string;
  /** The kept part's tint and outline (the marker unless a theme says). */
  region: string;
  label: string;
  grid: string;
  font: string;
}

export function readPalette(element: Element): WavePalette {
  const style = getComputedStyle(element);
  const token = (name: string, fallback: string) => style.getPropertyValue(name).trim() || fallback;
  const ink = token('--transport-display-ink', '#e8ece2');
  return {
    wave: alpha(ink, 0.55),
    kept: alpha(ink, 0.92),
    playhead: token('--transport-playhead', ink),
    marker: token('--transport-marker', '#f07836'),
    region: token('--transport-region', token('--transport-marker', '#f07836')),
    label: token('--transport-muted', '#aaa9a2'),
    grid: alpha(token('--transport-track', '#4c4c47'), 0.7),
    font: style.getPropertyValue('--font-ui').trim() || 'system-ui, sans-serif'
  };
}

/** A `#rrggbb` or `rgb()` colour (as the themes' tokens are) at an opacity. */
export function alpha(color: string, opacity: number) {
  const hex = /^#([0-9a-f]{6})$/i.exec(color);
  if (hex) {
    const value = Number.parseInt(hex[1], 16);
    return `rgba(${value >> 16}, ${(value >> 8) & 255}, ${value & 255}, ${opacity})`;
  }
  const rgb = /^rgba?\(\s*(\d+)[\s,]+(\d+)[\s,]+(\d+)/i.exec(color);
  return rgb ? `rgba(${rgb[1]}, ${rgb[2]}, ${rgb[3]}, ${opacity})` : color;
}

/** Sizes `canvas` to `width` x `height` CSS pixels at the screen's density
 * and returns a context drawing in CSS pixels. */
export function prepare(canvas: HTMLCanvasElement, width: number, height: number) {
  const scale = window.devicePixelRatio || 1;
  const pixelWidth = Math.max(1, Math.round(width * scale));
  const pixelHeight = Math.max(1, Math.round(height * scale));
  if (canvas.width !== pixelWidth) canvas.width = pixelWidth;
  if (canvas.height !== pixelHeight) canvas.height = pixelHeight;
  const context = canvas.getContext('2d');
  if (!context) return undefined;
  context.setTransform(scale, 0, 0, scale, 0, 0);
  context.clearRect(0, 0, width, height);
  return context;
}

/** The waveform from `start` over `seconds`, across `width` x `height`, as
 * a filled outline: along the highest sample of each pixel column left to
 * right, back along the lowest. Amplitude is linear, full scale at the edges. */
export function wavePath(waveform: ClipWaveform, start: number, seconds: number, width: number, height: number) {
  const path = new Path2D();
  const columns = Math.max(1, Math.floor(width));
  const pairsPerSecond = waveform.sample_rate / waveform.samples_per_pixel;
  const middle = height / 2;
  const reach = Math.max(1, middle - 1);
  const tops: number[] = [];
  const bottoms: number[] = [];
  for (let x = 0; x < columns; x += 1) {
    const from = Math.floor((start + (x / columns) * seconds) * pairsPerSecond);
    const to = Math.max(from + 1, Math.floor((start + ((x + 1) / columns) * seconds) * pairsPerSecond));
    if (from >= waveform.length) break;
    let low = 0;
    let high = 0;
    for (let index = Math.max(0, from); index < Math.min(to, waveform.length); index += 1) {
      low = Math.min(low, waveform.data[index * 2]);
      high = Math.max(high, waveform.data[index * 2 + 1]);
    }
    tops.push(middle - (high / 128) * reach);
    // At least a hairline, so silence still reads as a line.
    bottoms.push(Math.max(middle + 0.5, middle - (low / 128) * reach));
  }
  if (tops.length === 0) return path;
  path.moveTo(0, Math.min(tops[0], middle - 0.5));
  tops.forEach((y, x) => path.lineTo(x + 0.5, Math.min(y, middle - 0.5)));
  for (let x = bottoms.length - 1; x >= 0; x -= 1) path.lineTo(x + 0.5, bottoms[x]);
  path.closePath();
  return path;
}

/** Calls `draw(width, height, true)` whenever the canvas is resized, the
 * theme changes, or `deps` change. Returns the size, and `redraw` to draw
 * again now with the latest `draw` (a moving playhead, say), which passes
 * `changed` false: nothing but the moving part needs drawing again. */
export function useCanvasDrawing(canvas: RefObject<HTMLCanvasElement | null>, draw: (width: number, height: number, changed: boolean) => void, deps: unknown[]) {
  const size = useRef({ width: 0, height: 0 });
  const latest = useRef(draw);
  latest.current = draw;
  useEffect(() => {
    const element = canvas.current;
    if (!element) return;
    const observer = new ResizeObserver(([entry]) => {
      size.current = { width: entry.contentRect.width, height: entry.contentRect.height };
      latest.current(size.current.width, size.current.height, true);
    });
    observer.observe(element);
    // A new theme (or custom colours) changes the palette under the canvas.
    const theme = new MutationObserver(() => {
      if (size.current.width > 0) latest.current(size.current.width, size.current.height, true);
    });
    theme.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme', 'style'] });
    return () => {
      observer.disconnect();
      theme.disconnect();
    };
  }, [canvas]);
  useEffect(() => {
    if (size.current.width > 0) latest.current(size.current.width, size.current.height, true);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps);
  const redraw = useRef(() => {
    if (size.current.width > 0) latest.current(size.current.width, size.current.height, false);
  }).current;
  return { size, redraw };
}
