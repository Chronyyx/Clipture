interface PlaybackActivity {
  mixed: boolean;
  context(): AudioContext | null;
  buffer(): void;
  idle(): void;
}

/** No scheduler wakeups while paused; native audio and boosted audio share this. */
export function watchPlaybackActivity(video: HTMLVideoElement, activity: PlaybackActivity) {
  let timer: number | undefined;
  const clearTimer = () => {
    if (timer !== undefined) window.clearInterval(timer);
    timer = undefined;
  };
  const start = () => {
    clearTimer();
    if (video.paused || document.hidden) return;
    const context = activity.context();
    if (context?.state === 'suspended') void context.resume().catch(() => {});
    if (!activity.mixed) return;
    activity.buffer();
    timer = window.setInterval(activity.buffer, 100);
  };
  const idle = () => {
    clearTimer();
    activity.idle();
    const context = activity.context();
    if (context?.state === 'running') void context.suspend().catch(() => {});
  };
  video.addEventListener('play', start);
  video.addEventListener('pause', idle);
  video.addEventListener('ended', idle);
  start();
  return () => {
    video.removeEventListener('play', start);
    video.removeEventListener('pause', idle);
    video.removeEventListener('ended', idle);
    clearTimer();
  };
}
