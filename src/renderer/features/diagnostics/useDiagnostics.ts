import { useEffect, useReducer, useState } from 'react';
import { clipture } from '../../platform';
import { startVisibleInterval } from '../../shared/visibleInterval';
import { diagnosticsSnapshot, initialDiagnosticsSnapshot } from './diagnosticsSnapshot';

export function useDiagnostics(onNotice: (message: string, durationMs?: number) => void) {
  const [snapshot, dispatch] = useReducer(diagnosticsSnapshot, initialDiagnosticsSnapshot);
  const [isExportingDiagnostics, setIsExportingDiagnostics] = useState(false);
  useEffect(() => {
    let active = true;
    let pending = false;
    const refresh = async () => {
      if (pending) return;
      pending = true;
      try {
        const next = await clipture.getDiagnostics();
        if (active) dispatch({ type: 'received', diagnostics: next });
      } catch (error) {
        if (active) dispatch({ type: 'delayed', error });
      }
      finally { pending = false; }
    };
    void refresh();
    // Paused while the window is minimized; nothing reads it then.
    const stop = startVisibleInterval(() => void refresh(), 2000);
    return () => { active = false; stop(); };
  }, []);

  async function exportDiagnostics() {
    if (isExportingDiagnostics) return;
    setIsExportingDiagnostics(true);
    try {
      const filePath = await clipture.exportDiagnostics();
      if (filePath) onNotice(`Diagnostics exported to ${filePath}`, 5000);
    } catch (error) {
      onNotice(error instanceof Error ? error.message : 'Could not export diagnostics.', 6000);
    } finally { setIsExportingDiagnostics(false); }
  }
  return { diagnostics: snapshot.diagnostics, diagnosticsError: snapshot.error,
    hasDiagnostics: snapshot.received, exportDiagnostics, isExportingDiagnostics };
}
