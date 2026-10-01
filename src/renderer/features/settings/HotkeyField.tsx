import { useState } from 'react';
import { acceleratorFromKeyboardEvent } from './capture/accelerator';

// Click, then press the shortcut. Backspace/Delete clears it, Escape cancels.
export function HotkeyField({ id, value, onChange }: { id: string; value: string; onChange: (hotkey: string) => void }) {
  const [recording, setRecording] = useState(false);
  return (
    <button
      id={id}
      className={recording ? 'keybind-button recording' : 'keybind-button'}
      type='button'
      aria-live='polite'
      onBlur={() => setRecording(false)}
      onClick={(event) => {
        setRecording(true);
        event.currentTarget.focus();
      }}
      onKeyDown={(event) => {
        if (!recording) return;
        event.preventDefault();
        event.stopPropagation();
        if (event.key === 'Escape') {
          setRecording(false);
          return;
        }
        if (event.key === 'Backspace' || event.key === 'Delete') {
          onChange('');
          setRecording(false);
          return;
        }
        const accelerator = acceleratorFromKeyboardEvent(event);
        if (!accelerator) return;
        onChange(accelerator);
        setRecording(false);
      }}
    >
      {recording ? 'Press a shortcut…' : value || 'Not set'}
    </button>
  );
}
