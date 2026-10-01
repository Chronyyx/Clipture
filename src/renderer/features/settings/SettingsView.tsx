import { AudioLines, Bell, Disc3, HardDrive, MonitorPlay, Palette, type LucideIcon } from 'lucide-react';
import { AnimatePresence, motion } from 'motion/react';
import { useState } from 'react';
import type { ClipSettings, ClipSoundOption } from '../../../shared/types';
import { SpookyRailPet } from '../../shared/halloween';
import { CafeRailPet } from '../../shared/maid-cafe';
import { AudioSettings } from './audio/AudioSettings';
import { CustomizeSettings } from './appearance/CustomizeSettings';
import { NotificationSettings } from './NotificationSettings';
import { RecordingSettings } from './RecordingSettings';
import { StorageSettings } from './StorageSettings';
import { VideoSettings } from './VideoSettings';

type SectionId = 'recording' | 'video' | 'audio' | 'notifications' | 'storage' | 'appearance';

const sections: Array<{ id: SectionId; label: string; Icon: LucideIcon }> = [
  { id: 'recording', label: 'Recording', Icon: Disc3 },
  { id: 'video', label: 'Video', Icon: MonitorPlay },
  { id: 'audio', label: 'Audio', Icon: AudioLines },
  { id: 'notifications', label: 'Notifications', Icon: Bell },
  { id: 'storage', label: 'Storage', Icon: HardDrive },
  { id: 'appearance', label: 'Appearance', Icon: Palette }
];

// Remembered while the app runs, so returning to Settings reopens the same place.
let lastSection: SectionId = 'recording';

interface SettingsViewProps {
  settings: ClipSettings;
  clipSounds: ClipSoundOption[];
  onChange: (patch: Partial<ClipSettings>) => void;
  onPreviewSound: (sound: string) => void;
  onImportSound: () => void;
  onRevealSounds: () => void;
}

export function SettingsView({ settings, clipSounds, onChange, onPreviewSound, onImportSound, onRevealSounds }: SettingsViewProps) {
  const [active, setActive] = useState<SectionId>(lastSection);

  function select(id: SectionId) {
    lastSection = id;
    setActive(id);
  }

  return (
    <section className='settings-container'>
      <nav className='settings-rail' aria-label='Settings sections'>
        {sections.map(({ id, label, Icon }) => (
          <button key={id} type='button' className={active === id ? 'settings-rail-item active' : 'settings-rail-item'}
            aria-current={active === id ? 'page' : undefined} onClick={() => select(id)}>
            {active === id && (
              <motion.span layoutId='settings-rail-active' className='settings-rail-indicator'
                transition={{ type: 'spring', bounce: 0, visualDuration: 0.25 }} />
            )}
            <Icon size={17} aria-hidden='true' />
            <span>{label}</span>
          </button>
        ))}
        <CafeRailPet section={active} />
        <SpookyRailPet section={active} />
      </nav>
      <AnimatePresence mode='wait' initial={false}>
        <motion.div
          key={active}
          className='settings-content'
          initial={{ opacity: 0, transform: 'translateY(6px)' }}
          // Cleared afterwards: a transform would contain position:fixed dialogs.
          animate={{ opacity: 1, transform: 'translateY(0px)', transitionEnd: { transform: 'none' } }}
          exit={{ opacity: 0, transform: 'translateY(-4px)' }}
          transition={{ duration: 0.14, ease: 'easeOut' }}
        >
          {active === 'recording' && <RecordingSettings settings={settings} onChange={onChange} />}
          {active === 'video' && <VideoSettings settings={settings} onChange={onChange} />}
          {active === 'audio' && <AudioSettings settings={settings} onChange={onChange} />}
          {active === 'notifications' && (
            <NotificationSettings settings={settings} clipSounds={clipSounds} onChange={onChange}
              onPreviewSound={onPreviewSound} onImportSound={onImportSound} onRevealSounds={onRevealSounds} />
          )}
          {active === 'storage' && <StorageSettings settings={settings} onChange={onChange} />}
          {active === 'appearance' && <CustomizeSettings settings={settings} onChange={onChange} />}
        </motion.div>
      </AnimatePresence>
    </section>
  );
}
