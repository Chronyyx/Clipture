import type { ClipSettings } from '../../../shared/types';
import { clipture } from '../../platform';
import { ClipRepairPanel } from './ClipRepairPanel';
import { SettingRow, SettingsSection } from './SettingsSection';

interface StorageSettingsProps {
  settings: ClipSettings;
  onChange: (patch: Partial<ClipSettings>) => void;
}

export function StorageSettings({ settings, onChange }: StorageSettingsProps) {
  async function chooseFolder() {
    const folder = await clipture.selectFolder(settings.saveFolder);
    if (folder && folder !== settings.saveFolder) onChange({ saveFolder: folder });
  }

  return (
    <SettingsSection title='Storage' description='Where clips are saved and keeping older clips playable.'>
      <SettingRow
        title='Save folder'
        description={<span className='setting-path' title={settings.saveFolder}>{settings.saveFolder || 'Not chosen yet'}</span>}
        control={(id) => <button id={id} className='secondary-button' type='button' onClick={() => void chooseFolder()}>Change…</button>}
      />
      <ClipRepairPanel />
    </SettingsSection>
  );
}
