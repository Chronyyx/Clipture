import type { ClipSettings } from '../../shared/types';

// Old hosts/profiles omit the additive field. Explicit opt-out must survive.
export function normalizeSaveSettings(settings: ClipSettings): ClipSettings {
  return { ...settings, saveInPlace: settings.saveInPlace !== false };
}
