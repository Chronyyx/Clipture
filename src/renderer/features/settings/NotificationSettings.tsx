import type { ClipSettings, ClipSoundOption } from '../../../shared/types';
import { SettingRow, SettingsSection, Toggle } from './SettingsSection';

interface NotificationSettingsProps {
  settings: ClipSettings;
  clipSounds: ClipSoundOption[];
  onChange: (patch: Partial<ClipSettings>) => void;
  onPreviewSound: (sound: string) => void;
  onImportSound: () => void;
  onRevealSounds: () => void;
}

const positions: Array<[ClipSettings['notificationPosition'], string]> = [
  ['top-right', 'Top right'],
  ['top-center', 'Top center'],
  ['top-left', 'Top left'],
  ['bottom-right', 'Bottom right'],
  ['bottom-left', 'Bottom left']
];

export function NotificationSettings({ settings, clipSounds, onChange, onPreviewSound, onImportSound, onRevealSounds }: NotificationSettingsProps) {
  return (
    <SettingsSection title='Notifications' description='How Clipture tells you a clip was saved while you are in a game.'>
      <SettingRow
        title='Saved clip popup'
        description='A small preview appears over your game after each save.'
        control={(id) => <Toggle id={id} checked={settings.showNotification} onChange={(showNotification) => onChange({ showNotification })} />}
      />
      <SettingRow
        title='Popup position'
        disabled={!settings.showNotification}
        control={(id) => (
          <select id={id} disabled={!settings.showNotification} value={settings.notificationPosition || 'top-right'}
            onChange={(event) => onChange({ notificationPosition: event.target.value as ClipSettings['notificationPosition'] })}>
            {positions.map(([value, label]) => <option key={value} value={value}>{label}</option>)}
          </select>
        )}
      />
      <SettingRow
        title='Sound'
        description='Plays when a clip is saved. Choosing one plays a preview.'
        control={(id) => (
          <select id={id} value={settings.clipSound || 'none'} onChange={(event) => {
            onChange({ clipSound: event.target.value });
            onPreviewSound(event.target.value);
          }}>
            <option value='none'>No sound</option>
            {clipSounds.map((sound) => <option key={sound.id} value={sound.id}>{sound.label}</option>)}
          </select>
        )}
      />
      <SettingRow
        title='Your own sounds'
        description='Add an MP3 or WAV file, or manage the sounds folder.'
        control={() => (
          <span className='setting-row-buttons'>
            <button className='secondary-button' type='button' onClick={onImportSound}>Add sound</button>
            <button className='secondary-button' type='button' onClick={onRevealSounds}>Open folder</button>
          </span>
        )}
      />
    </SettingsSection>
  );
}
