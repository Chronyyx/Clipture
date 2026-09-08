import { X } from 'lucide-react';
import { useMemo, useState } from 'react';
import type { ActiveProcess, AudioSourceRule } from '../../../../shared/types';

interface SystemAudioModalProps {
  source: AudioSourceRule;
  activeProcesses: ActiveProcess[];
  otherAppProcesses: Set<string>;
  onSave: (patch: Partial<AudioSourceRule>) => void;
  onClose: () => void;
}

export function SystemAudioModal({
  source,
  activeProcesses,
  otherAppProcesses,
  onSave,
  onClose
}: SystemAudioModalProps) {
  const [captureAll, setCaptureAll] = useState(source.captureAllSystem ?? true);
  const [selectedProcesses, setSelectedProcesses] = useState(() => new Set(source.processNames ?? []));
  const [search, setSearch] = useState('');
  const normalizedSearch = search.toLowerCase();
  const visibleNames = useMemo(() => (
    Array.from(new Set([...activeProcesses.map((process) => process.name), ...selectedProcesses]))
      .filter((name) => name.toLowerCase().includes(normalizedSearch))
      .sort((left, right) => {
        const selectedOrder = Number(selectedProcesses.has(right)) - Number(selectedProcesses.has(left));
        return selectedOrder || left.localeCompare(right);
      })
  ), [activeProcesses, normalizedSearch, selectedProcesses]);
  const selectableNames = visibleNames.filter((name) => !otherAppProcesses.has(name));
  const hasSelectedVisibleProcess = selectableNames.some((name) => selectedProcesses.has(name));

  function toggleProcess(name: string) {
    setSelectedProcesses((current) => {
      const next = new Set(current);
      if (next.has(name)) next.delete(name);
      else next.add(name);
      return next;
    });
  }

  function toggleVisibleProcesses() {
    setSelectedProcesses((current) => {
      const next = new Set(current);
      for (const name of selectableNames) {
        if (hasSelectedVisibleProcess) next.delete(name);
        else next.add(name);
      }
      return next;
    });
  }

  return (
    <div className='modal-backdrop' onClick={onClose}>
      <div className='modal system-audio-modal' onClick={(event) => event.stopPropagation()}>
        <div className='modal-header'>
          <h2>Configure System Audio Mix</h2>
          <button className='icon-button' onClick={onClose}><X size={18} /></button>
        </div>
        <div className='modal-body'>
          <label className='radio-label'>
            <input type='radio' checked={captureAll} onChange={() => setCaptureAll(true)} />
            <span>Record entire system (All apps)</span>
          </label>
          <label className='radio-label'>
            <input type='radio' checked={!captureAll} onChange={() => setCaptureAll(false)} />
            <span>Record specific apps only</span>
          </label>

          {!captureAll && (
            <div className='process-list-container'>
              <div className='process-search-row'>
                <input
                  className='process-search'
                  value={search}
                  onChange={(event) => setSearch(event.target.value)}
                  placeholder='Search processes...'
                />
                <button className='secondary-button' onClick={toggleVisibleProcesses}>
                  {hasSelectedVisibleProcess ? 'Clear All' : 'Select All'}
                </button>
              </div>
              <div className='process-list'>
                {visibleNames.map((name) => {
                  const isSeparateTrack = otherAppProcesses.has(name);
                  const isOffline = !activeProcesses.some((process) => process.name === name);
                  return (
                    <label
                      key={name}
                      className={'process-item ' + (isSeparateTrack ? 'disabled' : '')}
                      title={isSeparateTrack ? 'This app is already configured as a separate track.' : ''}
                    >
                      <input
                        className='toggle-switch'
                        type='checkbox'
                        checked={selectedProcesses.has(name)}
                        disabled={isSeparateTrack}
                        onChange={() => toggleProcess(name)}
                      />
                      <span>{name} {isOffline && <span className='muted-inline'>(Offline)</span>}</span>
                      {isSeparateTrack && <span className='badge'>Separate Track</span>}
                    </label>
                  );
                })}
              </div>
            </div>
          )}
        </div>
        <div className='modal-footer'>
          <button onClick={onClose}>Cancel</button>
          <button
            className='primary'
            onClick={() => {
              onSave({ captureAllSystem: captureAll, processNames: Array.from(selectedProcesses) });
              onClose();
            }}
          >
            Save Configuration
          </button>
        </div>
      </div>
    </div>
  );
}
