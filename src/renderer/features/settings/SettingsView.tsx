import { Paintbrush } from 'lucide-react';
import { useState } from 'react';
import type { ClipSettings, ClipSoundOption } from '../../../shared/types';
import { GeneralSettings } from './GeneralSettings';
import { VideoSettings } from './VideoSettings';
import { CustomizeSettings } from './appearance/CustomizeSettings';
import { AudioSettings } from './audio/AudioSettings';

type SettingsCategory = 'general' | 'video' | 'audio' | 'customize';

interface SettingsViewProps {
  settings: ClipSettings;
  clipSounds: ClipSoundOption[];
  onChange: (patch: Partial<ClipSettings>) => void;
  onPreviewSound: (sound: string) => void;
  onImportSound: () => void;
  onRevealSounds: () => void;
}

export function SettingsView({
  settings,
  clipSounds,
  onChange,
  onPreviewSound,
  onImportSound,
  onRevealSounds
}: SettingsViewProps) {
  const [activeCategory, setActiveCategory] = useState<SettingsCategory>('general');

  return (
    <section className='settings-container'>
      <div className='settings-tabs'>
        <button className={activeCategory === 'general' ? 'settings-tab active' : 'settings-tab'} onClick={() => setActiveCategory('general')}>General</button>
        <button className={activeCategory === 'video' ? 'settings-tab active' : 'settings-tab'} onClick={() => setActiveCategory('video')}>Video</button>
        <button className={activeCategory === 'audio' ? 'settings-tab active' : 'settings-tab'} onClick={() => setActiveCategory('audio')}>Audio</button>
        <button className={activeCategory === 'customize' ? 'settings-tab active' : 'settings-tab'} onClick={() => setActiveCategory('customize')}>
          <Paintbrush size={16} /> Customize
        </button>
      </div>
      <div className='settings-content'>
        {activeCategory === 'general' && (
          <GeneralSettings
            settings={settings}
            clipSounds={clipSounds}
            onChange={onChange}
            onPreviewSound={onPreviewSound}
            onImportSound={onImportSound}
            onRevealSounds={onRevealSounds}
          />
        )}
        {activeCategory === 'video' && <VideoSettings settings={settings} onChange={onChange} />}
        {activeCategory === 'audio' && <AudioSettings settings={settings} onChange={onChange} />}
        {activeCategory === 'customize' && <CustomizeSettings settings={settings} onChange={onChange} />}
      </div>
    </section>
  );
}
