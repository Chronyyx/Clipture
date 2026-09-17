import { isHostBusyError } from '../../platform';

// Retry only explicit pre-dispatch admission failures. Never retry a timeout or
// arbitrary command error, which might have already created a media session.
export function preparePlayback<T>(
  request: () => Promise<T>,
  ready: (result: T) => void,
  failed: (error: unknown) => void
): () => void {
  let active = true;
  let attempt = 0;
  let timer: ReturnType<typeof setTimeout> | undefined;
  const delays = [100, 250, 500];
  const run = async () => {
    if (!active) return;
    try {
      const result = await request();
      if (active) ready(result);
    } catch (error) {
      if (!active) return;
      if (isHostBusyError(error) && attempt < delays.length) {
        timer = setTimeout(() => { void run(); }, delays[attempt++]);
      } else {
        failed(error);
      }
    }
  };
  void run();
  return () => { active = false; if (timer !== undefined) clearTimeout(timer); };
}
