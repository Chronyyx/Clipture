import type { EngineDiagnostics } from "../../shared/types";
import { defaultDiagnostics } from "../shared/diagnostics/defaultDiagnostics";

/**
 * Older engines and the Tauri migration host can legitimately report only the
 * diagnostics fields they know. Keep rendering deterministic while that
 * contract evolves by applying the partial snapshot over the UI defaults.
 */
export function mergeDiagnostics(value: unknown): EngineDiagnostics {
  if (!value || typeof value !== "object") return { ...defaultDiagnostics };
  const partial = value as Partial<EngineDiagnostics>;
  return {
    ...defaultDiagnostics,
    ...partial,
    lastClipCadence: {
      ...defaultDiagnostics.lastClipCadence,
      ...(partial.lastClipCadence ?? {}),
      buckets: partial.lastClipCadence?.buckets ?? defaultDiagnostics.lastClipCadence.buckets
    }
  };
}
