// The Aero shell's last finished bitmap, kept between loads. Parsing and
// laying out the shell's SVG is the slow part of showing Aero (the window
// is reopened with a fresh page every time), so a page that asks for the
// same art at the same size paints the stored bitmap instead.
//
// The key is a hash of the SVG text itself, which already encodes the size
// and pixel ratio: a change to the art, the window size or the scale misses
// the cache on its own, with no version to remember to bump.

const STORAGE_KEY = 'clipture.aeroShell';

type Stored = { key: string; image: string };

/** FNV-1a over the SVG text, with its length to spread collisions. */
export function shellKey(svg: string): string {
  let hash = 0x811c9dc5;
  for (let index = 0; index < svg.length; index += 1) {
    hash ^= svg.charCodeAt(index);
    hash = Math.imul(hash, 0x01000193);
  }
  return `${(hash >>> 0).toString(36)}-${svg.length.toString(36)}`;
}

/** The stored bitmap's data URL for `key`, if it is the one kept. */
export function cachedShell(key: string): string | undefined {
  try {
    const stored = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? 'null') as Stored | null;
    return stored?.key === key ? stored.image : undefined;
  } catch {
    return undefined;
  }
}

/** Drops the stored bitmap (one that would not decode). */
export function forgetShell(): void {
  try { localStorage.removeItem(STORAGE_KEY); } catch { /* storage unavailable */ }
}

/** Keeps `canvas` as the shell for `key`, replacing any other size. Runs
 * after the shell is on screen; a full store just keeps nothing. */
export function storeShell(key: string, canvas: HTMLCanvasElement): void {
  canvas.toBlob((blob) => {
    if (!blob) return;
    const reader = new FileReader();
    reader.onload = () => {
      try {
        localStorage.setItem(STORAGE_KEY, JSON.stringify({ key, image: reader.result as string }));
      } catch {
        forgetShell();
      }
    };
    reader.readAsDataURL(blob);
  }, 'image/webp', 0.95);
}
