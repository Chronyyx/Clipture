// The latest target per video while one of its seeks is still running.
const pending = new WeakMap<HTMLVideoElement, number>();

/** Moves `video` to `seconds` with at most one seek in flight. While a seek
 * runs, later targets replace each other and only the last is applied once it
 * lands. Dragging a trim handle asks for a seek on every pointer move; setting
 * each at once aborts the decode before a frame shows, and the picture stalls
 * until the drag stops. */
export function seekCoalesced(video: HTMLVideoElement, seconds: number) {
  if (!video.seeking) {
    pending.delete(video);
    if (Math.abs(video.currentTime - seconds) > 0.0005) video.currentTime = seconds;
    return;
  }
  const waiting = pending.has(video);
  pending.set(video, seconds);
  if (waiting) return;
  video.addEventListener('seeked', () => {
    const target = pending.get(video);
    if (target !== undefined) seekCoalesced(video, target);
  }, { once: true });
}
