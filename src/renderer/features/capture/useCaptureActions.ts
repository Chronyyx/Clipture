import { useEffect, useRef, useState } from 'react';
import type { ClipRecord, ClipSettings, SaveIoAnalyzerState } from '../../../shared/types';
import { clipture } from '../../platform';

export function useCaptureActions(
  settings: ClipSettings | undefined,
  onSaved: (clip: ClipRecord) => void,
  onSaveNotice: (message: string, durationMs?: number) => void,
  onTraceNotice: (message: string, durationMs?: number) => void
) {
  const [isSavingClip, setIsSavingClip] = useState(false);
  const busy = useRef(false);
  const [saveIoAnalyzer, setSaveIoAnalyzer] = useState<SaveIoAnalyzerState>({
    available: false, armed: false, traceReady: false
  });
  useEffect(() => {
    let active = true;
    void clipture.getSaveIoAnalyzerState().then(next => { if (active) setSaveIoAnalyzer(next); }).catch(console.warn);
    return () => { active = false; };
  }, []);

  async function saveClip() {
    if (busy.current) return;
    busy.current = true;
    setIsSavingClip(true);
    try {
      const result = await clipture.saveClip(settings?.clipLengthSeconds ?? 30);
      onSaveNotice(result.message + (result.saveIoAnalysis?.length ? ' I/O trace ready.' : ''));
      if (result.ok && result.clip) onSaved(result.clip);
    } catch (error) {
      onSaveNotice(error instanceof Error ? error.message : 'Could not save clip.', 6000);
    } finally {
      busy.current = false;
      setIsSavingClip(false);
      void clipture.getSaveIoAnalyzerState().then(setSaveIoAnalyzer).catch(console.warn);
    }
  }

  async function toggleSaveIoAnalyzer() {
    if (!saveIoAnalyzer.available || busy.current) return;
    try {
      const next = await clipture.setSaveIoAnalyzerArmed(!saveIoAnalyzer.armed);
      setSaveIoAnalyzer(next);
      onTraceNotice(next.armed ? 'Next save I/O trace armed' : 'I/O trace canceled');
    } catch (error) {
      onTraceNotice(error instanceof Error ? error.message : 'Could not change I/O tracing.', 6000);
    }
  }
  return { saveClip, isSavingClip, saveIoAnalyzer, toggleSaveIoAnalyzer };
}
