import { useEffect, useRef, useSyncExternalStore } from 'react';
import { startWindowResize, watchWindowFill, type ResizeEdge } from '../../platform';
import { SHELL_SLICES, drawAeroShell } from './shellArt';
import { cachedShell, forgetShell, shellKey, storeShell } from './shellCache';

function subscribe(onChange: () => void) {
  const observer = new MutationObserver(onChange);
  observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
  return () => observer.disconnect();
}

/** True while the Aero theme is applied (saved or previewed). */
export function useAeroTheme(): boolean {
  return useSyncExternalStore(subscribe, () => document.documentElement.dataset.theme === 'aero');
}

/** The Aero theme's moulded shell, drawn behind the interface. Mounts only
 * while the Aero theme is applied (saved or previewed). */
export function AeroShell() {
  return useAeroTheme() ? <ShellArt /> : null;
}

/** Marks the page while the window fills the screen; only then is the
 * meadow drawn round the shell. Otherwise the desktop itself shows. */
function useWindowFill() {
  useEffect(() => {
    const root = document.documentElement;
    const stop = watchWindowFill((fills) => { root.dataset.windowFill = String(fills); });
    return () => {
      stop();
      delete root.dataset.windowFill;
    };
  }, []);
}

// Room round the shell for its shadow, in CSS pixels; aero.css matches it.
const SHADOW_ROOM = 24;
// How long the size must hold still before the art is redrawn sharp.
const SETTLE_MS = 120;

/** The art is rasterised once per settled size and kept. While the window
 * is being resized each frame re-cuts that bitmap in nine: corners and
 * curves are copied as they are and only the straight runs between them
 * stretch, nine image copies a frame. Redrawing the vector art every frame
 * is what stutters; it is redrawn sharp once the size holds still. */
function ShellArt() {
  useWindowFill();
  const host = useRef<HTMLDivElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const element = host.current;
    const target = canvas.current;
    if (!element || !target) return;
    let timer = 0;
    let run = 0;
    let drawn = '';
    const cache = document.createElement('canvas');
    let cached: { width: number; height: number; ratio: number } | undefined;

    const stretch = () => {
      if (!cached) return;
      // The stretched copy is only a stand-in: the next settle must redraw,
      // even when the window ends at the size last drawn sharp.
      drawn = '';
      const { width, height } = element.getBoundingClientRect();
      const ratio = window.devicePixelRatio || 1;
      if (width < 1 || height < 1) return;
      const room = SHADOW_ROOM;
      const { left, top, right, bottom } = SHELL_SLICES;
      const cuts = (size: number, near: number, far: number) => [0, room + near, room + size - far, room * 2 + size];
      const sx = cuts(cached.width, left, right).map((value) => value * cached!.ratio);
      const sy = cuts(cached.height, top, bottom).map((value) => value * cached!.ratio);
      const dx = cuts(width, left, right).map((value) => value * ratio);
      const dy = cuts(height, top, bottom).map((value) => value * ratio);
      target.width = Math.round(dx[3]);
      target.height = Math.round(dy[3]);
      const context = target.getContext('2d');
      if (!context) return;
      context.imageSmoothingQuality = 'high';
      for (let column = 0; column < 3; column += 1) {
        for (let row = 0; row < 3; row += 1) {
          const sw = sx[column + 1] - sx[column], sh = sy[row + 1] - sy[row];
          const dw = dx[column + 1] - dx[column], dh = dy[row + 1] - dy[row];
          if (sw > 0 && sh > 0 && dw > 0 && dh > 0) context.drawImage(cache, sx[column], sy[row], sw, sh, dx[column], dy[row], dw, dh);
        }
      }
    };

    const draw = async () => {
      const { width, height } = element.getBoundingClientRect();
      const ratio = window.devicePixelRatio || 1;
      const key = `${width}x${height}@${ratio}`;
      if (width < 1 || height < 1 || key === drawn) return;
      const current = ++run;
      // Built from the module's own constants; nothing user-supplied enters it.
      const svg = drawAeroShell(width, height, ratio);
      const shell = shellKey(svg);
      const stored = cachedShell(shell);
      const url = stored ?? URL.createObjectURL(new Blob([svg], { type: 'image/svg+xml' }));
      try {
        const image = new Image();
        image.src = url;
        await image.decode();
        if (current !== run) return;
        const room = SHADOW_ROOM * ratio;
        target.width = Math.round(width * ratio + room * 2);
        target.height = Math.round(height * ratio + room * 2);
        const context = target.getContext('2d');
        if (!context) return;
        if (stored) {
          // The stored bitmap is the finished canvas, shadow and all.
          context.drawImage(image, 0, 0, target.width, target.height);
        } else {
          context.shadowColor = 'rgb(4 12 60 / 50%)';
          context.shadowBlur = 9 * ratio;
          context.shadowOffsetY = 5 * ratio;
          context.drawImage(image, room, room, width * ratio, height * ratio);
          storeShell(shell, target);
        }
        cache.width = target.width;
        cache.height = target.height;
        cache.getContext('2d')?.drawImage(target, 0, 0);
        cached = { width, height, ratio };
        drawn = key;
        // The interface is held hidden until its shell is first drawn.
        document.documentElement.dataset.shellReady = 'true';
      } catch {
        // A stored bitmap that will not decode is dropped and the art drawn
        // afresh; a failed fresh decode leaves the last good art in place.
        if (stored && current === run) {
          forgetShell();
          void draw();
        }
      } finally {
        if (!stored) URL.revokeObjectURL(url);
      }
    };

    const observer = new ResizeObserver(() => {
      // Re-cut now, in the frame whose layout changed: a frame later the
      // page would already sit at the new size over the old art.
      if (cached) stretch();
      window.clearTimeout(timer);
      timer = window.setTimeout(() => void draw(), drawn ? SETTLE_MS : 0);
    });
    observer.observe(element);
    // A media query matches one pixel ratio, so after each change (a move to
    // another monitor) it is replaced by one for the new ratio.
    let scaled: MediaQueryList | undefined;
    const watchScale = () => {
      scaled?.removeEventListener('change', rescale);
      scaled = window.matchMedia(`(resolution: ${window.devicePixelRatio || 1}dppx)`);
      scaled.addEventListener('change', rescale);
    };
    function rescale() {
      watchScale();
      void draw();
    }
    watchScale();
    return () => {
      observer.disconnect();
      scaled?.removeEventListener('change', rescale);
      window.clearTimeout(timer);
      run += 1;
      delete document.documentElement.dataset.shellReady;
    };
  }, []);

  return (
    <>
      <div ref={host} className="aero-shell" aria-hidden="true">
        <canvas ref={canvas} />
      </div>
      <ResizeGrips />
    </>
  );
}

const GRIPS: ResizeEdge[] = ['North', 'South', 'East', 'West', 'NorthEast', 'NorthWest', 'SouthEast', 'SouthWest'];

/** The window is cut to the shell's outline, which takes away the system's
 * resize borders; these grips along the shell's own rim and corners hand
 * the pointer back to the system resize. */
function ResizeGrips() {
  return (
    <div className="aero-resize" aria-hidden="true">
      {GRIPS.map((edge) => (
        <span key={edge} className={`aero-resize-${edge.toLowerCase()}`}
          onPointerDown={(event) => {
            if (event.button !== 0) return;
            event.preventDefault();
            void startWindowResize(edge).catch(() => undefined);
          }} />
      ))}
    </div>
  );
}
