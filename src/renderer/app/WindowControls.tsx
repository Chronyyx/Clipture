import { controlMainWindow, type WindowAction } from "../platform";

const controls: Array<{ action: WindowAction; label: string; glyph: string }> = [
  { action: "close", label: "Close", glyph: "M3.5 3.5l5 5M8.5 3.5l-5 5" },
  { action: "minimize", label: "Minimize", glyph: "M3 6h6" },
  { action: "maximize", label: "Maximize", glyph: "M3 3h3.5L3 6.5zM9 9H5.5L9 5.5z" }
];

/** Close, minimize and maximize as three lights in the window's corner, for
 * themes drawn without the system frame. Each shows its glyph while it is
 * pointed at. */
export function WindowControls() {
  return (
    <div className="window-controls" role="group" aria-label="Window">
      {controls.map(({ action, label, glyph }) => (
        <button key={action} type="button" className={`window-control ${action}`} aria-label={label} title={label}
          onClick={() => void controlMainWindow(action).catch((error) => console.error(error))}>
          <svg viewBox="0 0 12 12" aria-hidden="true">
            <path d={glyph} />
          </svg>
        </button>
      ))}
    </div>
  );
}
