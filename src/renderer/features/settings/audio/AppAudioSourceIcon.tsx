import { useEffect, useState } from 'react';
import type { AudioSourceRule } from '../../../../shared/types';
import { clipture } from '../../../platform';

export function AppAudioSourceIcon({ source, fallback }: { source: AudioSourceRule; fallback: string }) {
  const [iconUrl, setIconUrl] = useState('');
  const processName = source.processName || '';
  const executablePath = source.executablePath || '';

  useEffect(() => {
    let active = true;
    setIconUrl('');
    if (!processName) return () => { active = false; };

    void clipture.processIconUrl(processName, executablePath).then((url) => {
      if (active) setIconUrl(url || '');
    }).catch(() => {
      if (active) setIconUrl('');
    });

    return () => { active = false; };
  }, [processName, executablePath]);

  return (
    <span className={'audio-source-icon audio-app-icon' + (iconUrl ? ' has-image' : '')} aria-hidden='true'>
      {iconUrl ? <img className='audio-app-icon-image' src={iconUrl} alt='' /> : fallback}
    </span>
  );
}
