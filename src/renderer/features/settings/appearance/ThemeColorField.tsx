import { useEffect, useRef, useState } from 'react';
import { normalizeThemeColor } from '../../../theme';

interface ThemeColorFieldProps {
  label: string;
  value: string;
  onPreview: (value: string) => void;
  onCommit: (value: string) => void;
}

export function ThemeColorField({ label, value, onPreview, onCommit }: ThemeColorFieldProps) {
  const [draft, setDraft] = useState(value);
  const [editing, setEditing] = useState(false);
  const cancelCommit = useRef(false);

  useEffect(() => {
    if (!editing) setDraft(value);
  }, [editing, value]);

  function commit(candidate: string) {
    const withHash = candidate.startsWith('#') ? candidate : '#' + candidate;
    const normalized = normalizeThemeColor(withHash, value);
    setDraft(normalized);
    onPreview(normalized);
    if (normalized !== value) onCommit(normalized);
  }

  return (
    <label className='theme-color-field'>
      <span>{label}</span>
      <span className='theme-color-control'>
        <input
          aria-label={'Choose ' + label.toLowerCase()}
          className='theme-color-picker'
          type='color'
          value={normalizeThemeColor(draft, value)}
          onChange={(event) => {
            const color = event.currentTarget.value.toLowerCase();
            setDraft(color);
            onPreview(color);
          }}
          onBlur={(event) => commit(event.currentTarget.value)}
        />
        <input
          aria-label={label + ' hex value'}
          className='theme-color-hex'
          maxLength={7}
          spellCheck={false}
          value={draft}
          onFocus={() => setEditing(true)}
          onChange={(event) => {
            const next = event.currentTarget.value;
            if (!/^#?[0-9a-f]{0,6}$/i.test(next)) return;
            setDraft(next);
            const withHash = next.startsWith('#') ? next : '#' + next;
            if (/^#[0-9a-f]{6}$/i.test(withHash)) onPreview(withHash.toLowerCase());
          }}
          onBlur={() => {
            setEditing(false);
            if (cancelCommit.current) {
              cancelCommit.current = false;
              return;
            }
            commit(draft);
          }}
          onKeyDown={(event) => {
            if (event.key === 'Enter') event.currentTarget.blur();
            if (event.key === 'Escape') {
              cancelCommit.current = true;
              setDraft(value);
              onPreview(value);
              event.currentTarget.blur();
            }
          }}
        />
      </span>
    </label>
  );
}
