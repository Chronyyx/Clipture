import { X } from 'lucide-react';
import { useState } from 'react';
import type { ActiveProcess, AudioSourceRule } from '../../../../shared/types';

interface AppAudioModalProps {
  source: AudioSourceRule;
  activeProcesses: ActiveProcess[];
  loading?: boolean;
  onSave: (patch: Partial<AudioSourceRule>) => void;
  onClose: () => void;
}

export function AppAudioModal({ source, activeProcesses, loading = false, onSave, onClose }: AppAudioModalProps) {
  const [search, setSearch] = useState('');
  const filteredProcesses = activeProcesses.filter((process) =>
    process.name.toLowerCase().includes(search.toLowerCase())
  );

  return (
    <div className='modal-backdrop' onClick={onClose}>
      <div className='modal system-audio-modal' onClick={(event) => event.stopPropagation()}>
        <div className='modal-header'>
          <h2>Choose an app</h2>
          <button className='icon-button' onClick={onClose}><X size={18} /></button>
        </div>
        <div className='modal-body'>
          <input
            className='process-search'
            value={search}
            onChange={(event) => setSearch(event.target.value)}
            placeholder='Search running apps'
            autoFocus
          />
          <div className='process-list process-picker-list' aria-busy={loading}>
            {loading && [0, 1, 2, 3].map((row) => <span key={row} className='skeleton process-skeleton' aria-hidden='true' />)}
            {filteredProcesses.map((process) => (
              <label key={process.name + '-' + process.pid} className='process-item'>
                <input
                  type='radio'
                  name='app-process'
                  checked={source.processName === process.name}
                  onChange={() => onSave({
                    label: process.name || 'App audio',
                    processName: process.name,
                    executablePath: process.executablePath,
                    enabled: true
                  })}
                />
                <span>{process.name}</span>
              </label>
            ))}
          </div>
        </div>
      </div>
    </div>
  );
}
