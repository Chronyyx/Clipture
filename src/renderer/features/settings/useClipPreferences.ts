import { useCallback, useEffect, useRef, useState } from 'react';
import type { ClipSettings, ClipSoundOption } from '../../../shared/types';
import { clipture } from '../../platform';
import { cacheAndApplyUiTheme } from '../../theme';

export function useClipPreferences(onNotice: (message: string, durationMs?: number) => void) {
  const [settings, setSettings] = useState<ClipSettings>();
  const [clipSounds, setClipSounds] = useState<ClipSoundOption[]>([]);
  const current = useRef<ClipSettings>();
  const soundUrls = useRef<Record<string, string>>({});
  const writes = useRef<Promise<void>>(Promise.resolve());
  const reportError = useCallback((error: unknown) => {
    onNotice(error instanceof Error ? error.message : 'Could not update preferences.', 6000);
  }, [onNotice]);

  const refreshPreferences = useCallback(() => {
    // Serialize reads with edits so a slow refresh cannot revert a saved edit.
    const operation = writes.current.then(async () => {
    const [next, sounds] = await Promise.all([clipture.getSettings(), clipture.listClipSounds()]);
    current.current = next;
    setSettings(next);
    setClipSounds(sounds);
    soundUrls.current = Object.fromEntries(sounds.filter(sound => sound.url).map(sound => [sound.id, sound.url!]));
    cacheAndApplyUiTheme(next);
    });
    writes.current = operation.catch(() => {});
    return operation;
  }, []);

  const previewClipSound = useCallback((sound: string) => {
    const url = soundUrls.current[sound];
    if (!url || sound === 'none') return;
    void new Audio(url).play().catch(reportError);
  }, [reportError]);

  useEffect(() => {
    void refreshPreferences().catch(reportError);
    return clipture.onPlaySound(previewClipSound);
  }, [previewClipSound, refreshPreferences, reportError]);

  function updateSettings(patch: Partial<ClipSettings>): Promise<void> {
    // Each patch applies to the last acknowledged snapshot; quick edits cannot
    // overwrite an unrelated field with an older render's settings object.
    const operation = writes.current.then(async () => {
      if (!current.current) return;
      const next = { ...current.current, ...patch };
      const saved = await clipture.saveSettings(next);
      current.current = saved;
      setSettings(saved);
      cacheAndApplyUiTheme(saved);
      onNotice('Settings saved', 2200);
    });
    writes.current = operation.catch(reportError);
    return writes.current;
  }

  async function importClipSound() {
    try {
      const sound = await clipture.importClipSound();
      if (!sound) return;
      await refreshPreferences();
      await updateSettings({ clipSound: sound.id });
      previewClipSound(sound.id);
    } catch (error) { reportError(error); }
  }
  return { settings, clipSounds, updateSettings, previewClipSound, importClipSound,
    refreshPreferences, revealSounds: () => clipture.revealSoundsFolder() };
}
