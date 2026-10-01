import type { ClipSettings } from '../../../shared/types';
import { DraftNumberInput } from './DraftNumberInput';
import { HotkeyField } from './HotkeyField';
import { SettingRow, SettingsSection, Toggle } from './SettingsSection';

interface RecordingSettingsProps {
  settings: ClipSettings;
  onChange: (patch: Partial<ClipSettings>) => void;
}

function overlapDescription(settings: ClipSettings) {
  if (!settings.saveInPlace) return 'Always on while Save in place is off.';
  return settings.saveInPlaceOverlap
    ? 'Every save covers the full clip length, even right after another save.'
    : 'Each save starts where the previous one ended.';
}

export function RecordingSettings({ settings, onChange }: RecordingSettingsProps) {
  return (
    <SettingsSection title='Recording' description='Clipture keeps the last moments of your screen ready to save.'>
      <SettingRow
        title='Save shortcut'
        description='Press it in any game to save a clip. Click, then press new keys to change it.'
        control={(id) => <HotkeyField id={id} value={settings.hotkey} onChange={(hotkey) => onChange({ hotkey })} />}
      />
      <SettingRow
        title='Clip length'
        description='How far back each save reaches, from 5 seconds to 10 minutes.'
        control={(id) => (
          <span className='input-with-unit'>
            <DraftNumberInput id={id} min={5} max={600} value={settings.clipLengthSeconds} onCommit={(clipLengthSeconds) => onChange({ clipLengthSeconds })} />
            <span aria-hidden='true'>sec</span>
          </span>
        )}
      />
      <SettingRow
        title='Start with Windows'
        description='Starts hidden in the tray, so the replay buffer is always running.'
        control={(id) => <Toggle id={id} checked={settings.startOnLogin} onChange={(startOnLogin) => onChange({ startOnLogin })} />}
      />
      <SettingRow
        title='Save in place'
        description="Records to the save folder's drive so saving is instant and uses little memory."
        control={(id) => <Toggle id={id} checked={settings.saveInPlace} onChange={(saveInPlace) => onChange({ saveInPlace })} />}
      />
      <SettingRow
        title='Overlapping clips'
        description={overlapDescription(settings)}
        disabled={!settings.saveInPlace}
        control={(id) => (
          <Toggle id={id} checked={settings.saveInPlaceOverlap} disabled={!settings.saveInPlace}
            onChange={(saveInPlaceOverlap) => onChange({ saveInPlaceOverlap })} />
        )}
      />
    </SettingsSection>
  );
}
