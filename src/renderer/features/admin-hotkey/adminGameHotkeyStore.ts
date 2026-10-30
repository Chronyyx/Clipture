import { useCallback, useSyncExternalStore } from 'react';
import type { AdminGameHotkey, AdminGameHotkeyChange } from '../../../shared/types';
import { clipture } from '../../platform';

export type AdminGameHotkeyPhase = 'idle' | 'waiting' | AdminGameHotkeyChange['outcome'];

interface Snapshot {
  state: AdminGameHotkey | null;
  phase: AdminGameHotkeyPhase;
  message: string;
}

// One poll for every view that shows it (the card and its setting), only
// while one is on screen. The host keeps the truth; this only mirrors it.
const POLL_MS = 2000;
let snapshot: Snapshot = { state: null, phase: 'idle', message: '' };
const listeners = new Set<() => void>();
let timer: number | undefined;

function publish(next: Partial<Snapshot>) {
  snapshot = { ...snapshot, ...next };
  listeners.forEach((listener) => listener());
}

async function refresh() {
  try { publish({ state: await clipture.getAdminGameHotkey() }); }
  catch { /* A missing host leaves the card and setting hidden. */ }
}

function subscribe(listener: () => void) {
  listeners.add(listener);
  if (listeners.size === 1) {
    void refresh();
    timer = window.setInterval(() => void refresh(), POLL_MS);
  }
  return () => {
    listeners.delete(listener);
    if (listeners.size === 0) window.clearInterval(timer);
  };
}

async function change(run: () => Promise<AdminGameHotkeyChange>, prompts: boolean) {
  publish({ phase: prompts ? 'waiting' : snapshot.phase, message: '' });
  try {
    const result = await run();
    publish({ phase: result.outcome, message: result.message ?? '' });
  } catch (reason) {
    publish({ phase: 'failed', message: reason instanceof Error ? reason.message : String(reason) });
  }
  await refresh();
}

export function useAdminGameHotkey() {
  const current = useSyncExternalStore(subscribe, () => snapshot);
  const allow = useCallback(() => change(() => clipture.allowAdminGameHotkey(), !snapshot.state?.installed), []);
  const disable = useCallback(() => change(() => clipture.disableAdminGameHotkey(), false), []);
  const remove = useCallback(() => change(() => clipture.removeAdminGameHotkey(), true), []);
  const dismiss = useCallback(async () => {
    await clipture.dismissAdminGameHotkey().catch(() => undefined);
    await refresh();
  }, []);
  return { ...current, allow, disable, remove, dismiss };
}

/** The hotkey cannot reach the admin game in front, so it is worth asking. */
export function needsAdminGameHotkey(state: AdminGameHotkey | null) {
  if (!state?.game) return false;
  return !state.enabled || state.relay === 'outdated' || state.relay === 'not-installed' || state.relay === 'failed';
}
