/** What `followPlayback` schedules frames with (the window, or a test double). */
export interface FrameScheduler {
  requestAnimationFrame(callback: FrameRequestCallback): number;
  cancelAnimationFrame(handle: number): void;
}

const WAKE_EVENTS = ['play', 'seeked', 'timeupdate'] as const;

/** Calls `onFrame` every animation frame while `video` plays, and once after
 * it seeks or reports a new time while paused. However many events arrive,
 * at most one frame is pending, so the work never stacks up over a long
 * playback. Returns a function that stops following. */
export function followPlayback(video: HTMLVideoElement, onFrame: () => void, frames: FrameScheduler = window) {
  let pending: number | undefined;
  const tick = () => {
    pending = undefined;
    onFrame();
    if (!video.paused) schedule();
  };
  const schedule = () => {
    if (pending === undefined) pending = frames.requestAnimationFrame(tick);
  };
  for (const name of WAKE_EVENTS) video.addEventListener(name, schedule);
  if (!video.paused) schedule();
  return () => {
    if (pending !== undefined) frames.cancelAnimationFrame(pending);
    pending = undefined;
    for (const name of WAKE_EVENTS) video.removeEventListener(name, schedule);
  };
}
