import type { SharedClip, SharingSnapshot } from '../../shared/sharing';

/** Browser preview of a friend's side of a send: they answer, then watch a
 * little and download the rest. Jo always declines, so both endings can be
 * previewed; offline friends never answer. */
export function simulateFriend(state: () => SharingSnapshot, shareId: string, changed: () => void) {
  const share = () => state().outbox.find((clip) => clip.shareId === shareId);
  const presence = () => state().friends.find((friend) => friend.id === share()?.friendId)?.presence;
  if (presence() !== 'online') return;
  const timers: number[] = [];
  const later = (ms: number, step: (clip: SharedClip) => void) => {
    timers.push(window.setTimeout(() => {
      const clip = share();
      if (!clip) return timers.forEach((timer) => window.clearTimeout(timer));
      step(clip);
      changed();
    }, ms));
  };
  later(900, (clip) => { clip.delivered = true; });
  if (share()?.friendName === 'Jo') {
    later(3200, (clip) => { clip.answer = 'declined'; });
    return;
  }
  later(3200, (clip) => {
    clip.answer = 'accepted';
    clip.availableUntilMs = Date.now() + 15 * 60_000;
  });
  const steps = 24;
  for (let step = 1; step <= steps; step += 1) {
    later(4400 + step * 320 + (step > 14 ? 2400 : 0), (clip) => {
      const keep = step > 6;
      // A brief drop partway through, to show the reconnect state.
      const dropped = step === 14;
      clip.transfer = {
        sentBytes: Math.round(clip.size * step / steps),
        totalBytes: clip.size,
        purpose: keep ? 'keep' : 'watch',
        state: step === steps ? 'complete' : dropped ? 'interrupted' : 'active',
        bytesPerSecond: dropped || step === steps ? 0 : Math.round(clip.size / steps / 0.32)
      };
    });
  }
  // Their copy passes its check and the share closes.
  later(4400 + (steps + 3) * 320 + 2400, (clip) => { clip.saved = true; });
}
