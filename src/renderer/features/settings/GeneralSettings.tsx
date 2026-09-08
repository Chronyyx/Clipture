import { useState } from 'react';
import type { ClipSettings, ClipSoundOption } from '../../../shared/types';
import { clipture } from '../../platform';
import { acceleratorFromKeyboardEvent } from './capture/accelerator';

interface GeneralSettingsProps {
  settings: ClipSettings;
  clipSounds: ClipSoundOption[];
  onChange: (patch: Partial<ClipSettings>) => void;
  onPreviewSound: (sound: string) => void;
  onImportSound: () => void;
  onRevealSounds: () => void;
}

export function GeneralSettings({
  settings,
  clipSounds,
  onChange,
  onPreviewSound,
  onImportSound,
  onRevealSounds
}: GeneralSettingsProps) {
  const [recordingHotkey, setRecordingHotkey] = useState(false);

  return (
    <div className='settings-group'>
      <label>
        Hotkey
        <button
          className={recordingHotkey ? 'keybind-button recording' : 'keybind-button'}
          onBlur={() => setRecordingHotkey(false)}
          onClick={(event) => {
            setRecordingHotkey(true);
            event.currentTarget.focus();
          }}
          onKeyDown={(event) => {
            event.preventDefault();
            event.stopPropagation();
            if (event.key === 'Escape') {
              setRecordingHotkey(false);
              return;
            }
            if (event.key === 'Backspace' || event.key === 'Delete') {
              void onChange({ hotkey: '' });
              setRecordingHotkey(false);
              return;
            }
            const accelerator = acceleratorFromKeyboardEvent(event);
            if (!accelerator) return;
            void onChange({ hotkey: accelerator });
            setRecordingHotkey(false);
          }}
          type='button'
        >
          {recordingHotkey ? 'Press shortcut' : settings.hotkey || 'Unassigned'}
        </button>
      </label>
      <label className='wide'>
        Save folder
        <input
          readOnly
          className='folder-picker-input'
          title='Click to select a new folder'
          value={settings.saveFolder}
          onClick={async () => {
            const folder = await clipture.selectFolder(settings.saveFolder);
            if (folder && folder !== settings.saveFolder) void onChange({ saveFolder: folder });
          }}
        />
      </label>
      <label className='wide'>
        Clip sound cue
        <select
          value={settings.clipSound || 'none'}
          onChange={(event) => {
            const sound = event.target.value;
            onChange({ clipSound: sound });
            onPreviewSound(sound);
          }}
        >
          <option value='none'>None</option>
          {clipSounds.map((sound) => <option key={sound.id} value={sound.id}>{sound.label}</option>)}
        </select>
      </label>
      <div className='sound-actions wide'>
        <button className='secondary-button' type='button' onClick={onImportSound}>Import sound</button>
        <button className='secondary-button' type='button' onClick={onRevealSounds}>Open sounds folder</button>
      </div>
      <label className='toggle-label wide'>
        <input className='toggle-switch' type='checkbox' checked={settings.showNotification} onChange={(event) => onChange({ showNotification: event.target.checked })} />
        Show clip saved popup notification
      </label>
      <label className='wide'>
        Popup position
        <select value={settings.notificationPosition || 'top-right'} onChange={(event) => onChange({ notificationPosition: event.target.value as ClipSettings['notificationPosition'] })}>
          <option value='top-right'>Top Right</option>
          <option value='top-left'>Top Left</option>
          <option value='bottom-right'>Bottom Right</option>
          <option value='bottom-left'>Bottom Left</option>
          <option value='top-center'>Top Center</option>
        </select>
      </label>
      <label className='toggle-label wide'>
        <input className='toggle-switch' type='checkbox' checked={settings.startOnLogin} onChange={(event) => onChange({ startOnLogin: event.target.checked })} />
        Start silently on login
      </label>
    </div>
  );
}
