import type { ClipSettings } from '../../shared/types';
import { normalizeCaptureFps } from '../../shared/capture-fps';

// Old hosts/profiles omit the additive field. Explicit opt-out must survive.
export function normalizeSaveSettings(settings: ClipSettings): ClipSettings {
  return { ...settings, fps: normalizeCaptureFps(settings.fps), saveInPlace: settings.saveInPlace !== false };
}
