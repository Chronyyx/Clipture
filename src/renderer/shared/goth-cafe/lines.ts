export type GothRecorderState = 'live' | 'warning' | 'waiting' | 'off';

export const recorderLines: Record<GothRecorderState, string> = {
  live: 'Recording. Don’t make it cringe.',
  warning: 'Ugh. Something’s off. Still recording.',
  waiting: 'Finishing my eyeliner, one sec…',
  off: 'Recorder’s dead, babe.'
};

export function greeting(now = new Date()): string {
  const hour = now.getHours();
  if (hour < 5) return 'Up past midnight? Better.';
  if (hour < 12) return 'Morning. Ew.';
  return 'Oh. You’re back.';
}

export const peekerLines = ['Don’t stare.', 'Mrrp. Rude.', '…what.'];

export const emptyLines = {
  imported: 'Hand over a folder of videos, darling.',
  clips: 'Nothing here yet. Go clip something iconic.'
};

export const railLines: Record<string, string> = {
  recording: 'How far back should I remember? …zzz',
  video: 'Make it look good. Obviously.',
  audio: 'I hear everything.',
  notifications: 'Grim will tell you when it’s served.',
  storage: 'Where the clips are laid to rest.',
  appearance: 'Good taste. Welcome to the dark side.'
};
