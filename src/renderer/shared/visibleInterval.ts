/**
 * Calls `tick` every `intervalMs` while the page is visible, and not at all
 * while it is hidden (the window minimized or covered). On becoming visible
 * again it ticks at once, so what it shows is never a stale reading.
 * Returns a function that stops it.
 */
export function startVisibleInterval(tick: () => void, intervalMs: number): () => void {
  let timer: number | undefined;
  const start = () => {
    if (timer !== undefined) return;
    timer = window.setInterval(tick, intervalMs);
  };
  const stop = () => {
    if (timer === undefined) return;
    window.clearInterval(timer);
    timer = undefined;
  };
  const onVisibility = () => {
    if (document.visibilityState === 'hidden') {
      stop();
    } else {
      tick();
      start();
    }
  };
  if (document.visibilityState !== 'hidden') start();
  document.addEventListener('visibilitychange', onVisibility);
  return () => {
    stop();
    document.removeEventListener('visibilitychange', onVisibility);
  };
}
