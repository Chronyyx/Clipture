import type { KeyboardEvent } from 'react';

export function acceleratorFromKeyboardEvent(event: KeyboardEvent) {
  if (new Set(['Control', 'Shift', 'Alt', 'Meta', 'OS']).has(event.key)) return '';
  const parts: string[] = [];
  if (event.ctrlKey) parts.push('Ctrl');
  if (event.altKey) parts.push('Alt');
  if (event.shiftKey) parts.push('Shift');
  if (event.metaKey) parts.push('Super');
  const specialKeys: Record<string, string> = {
    ' ': 'Space',
    ArrowUp: 'Up',
    ArrowDown: 'Down',
    ArrowLeft: 'Left',
    ArrowRight: 'Right',
    Escape: 'Esc',
    '+': 'Plus'
  };
  const key = specialKeys[event.key] ?? (event.key.length === 1 ? event.key.toUpperCase() : event.key);
  if (!key || key === 'Esc') return '';
  parts.push(key);
  return parts.join('+');
}
