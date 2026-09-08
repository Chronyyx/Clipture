import { useEffect, useRef, useState } from 'react';

interface DraftNumberInputProps {
  value: number;
  min: number;
  max: number;
  disabled?: boolean;
  onCommit: (value: number) => void;
}

export function DraftNumberInput({ value, min, max, disabled = false, onCommit }: DraftNumberInputProps) {
  const [draft, setDraft] = useState(() => String(value));
  const cancelNextBlur = useRef(false);

  useEffect(() => setDraft(String(value)), [value]);

  function commitDraft() {
    const parsed = Number(draft.trim());
    if (!draft.trim() || !Number.isFinite(parsed)) {
      setDraft(String(value));
      return;
    }
    const nextValue = Math.min(max, Math.max(min, Math.round(parsed)));
    setDraft(String(nextValue));
    if (nextValue !== value) onCommit(nextValue);
  }

  return (
    <input
      type='text'
      inputMode='numeric'
      pattern='[0-9]*'
      value={draft}
      disabled={disabled}
      onChange={(event) => {
        if (/^\d*$/.test(event.target.value)) setDraft(event.target.value);
      }}
      onBlur={() => {
        if (cancelNextBlur.current) {
          cancelNextBlur.current = false;
          setDraft(String(value));
          return;
        }
        commitDraft();
      }}
      onKeyDown={(event) => {
        if (event.key === 'Enter') event.currentTarget.blur();
        if (event.key === 'Escape') {
          cancelNextBlur.current = true;
          setDraft(String(value));
          event.currentTarget.blur();
        }
      }}
    />
  );
}
