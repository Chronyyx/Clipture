import type { ClipSettings } from '../../shared/types';
import { normalizeCaptureFps } from '../../shared/capture-fps';

// Old hosts/profiles omit the additive fields. Explicit opt-outs must survive.
export function normalizeSaveSettings(settings: ClipSettings): ClipSettings {
  return {
    ...settings,
    fps: normalizeCaptureFps(settings.fps),
    saveInPlace: settings.saveInPlace !== false,
    saveInPlaceOverlap: settings.saveInPlaceOverlap !== false,
  };
}
