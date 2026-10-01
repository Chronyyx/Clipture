import { useId, type ReactNode } from 'react';

export function SettingsSection({ title, description, children }: { title: string; description?: string; children: ReactNode }) {
  const headingId = useId();
  return (
    <section className='settings-section' aria-labelledby={headingId}>
      <header className='settings-section-header'>
        <h2 id={headingId}>{title}</h2>
        {description && <p>{description}</p>}
      </header>
      <div className='settings-rows'>{children}</div>
    </section>
  );
}

interface SettingRowProps {
  title: string;
  description?: ReactNode;
  disabled?: boolean;
  /** Receives the id the title labels, so every control stays named. */
  control: (id: string) => ReactNode;
}

export function SettingRow({ title, description, disabled = false, control }: SettingRowProps) {
  const id = useId();
  return (
    <div className={disabled ? 'setting-row disabled' : 'setting-row'}>
      <div className='setting-row-copy'>
        <label className='setting-row-title' htmlFor={id}>{title}</label>
        {description && <p>{description}</p>}
      </div>
      <div className='setting-row-control'>{control(id)}</div>
    </div>
  );
}

export function Toggle({ id, checked, disabled, onChange }: { id: string; checked: boolean; disabled?: boolean; onChange: (checked: boolean) => void }) {
  return (
    <input id={id} className='toggle-switch' type='checkbox' role='switch' checked={checked} disabled={disabled}
      onChange={(event) => onChange(event.target.checked)} />
  );
}
