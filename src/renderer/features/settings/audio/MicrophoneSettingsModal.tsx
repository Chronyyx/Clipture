import { X } from 'lucide-react';
import type { AudioInputDevice, AudioSourceRule } from '../../../../shared/types';
import { TestMicButton } from './TestMicButton';
import {
  dbToGain,
  formatDb,
  gainToDb,
  maxMicGainDb,
  minMicGainDb,
  sensitivityFromThreshold,
  thresholdFromSensitivity
} from './audioMath';

interface MicrophoneSettingsModalProps {
  source: AudioSourceRule;
  inputDevices: AudioInputDevice[];
  onUpdate: (patch: Partial<AudioSourceRule>) => void;
  onClose: () => void;
}

export function MicrophoneSettingsModal({
  source,
  inputDevices,
  onUpdate,
  onClose
}: MicrophoneSettingsModalProps) {
  const volume = source.volume ?? 1;
  const volumeDb = gainToDb(volume);
  const voiceIsolation = source.voiceIsolation ?? false;
  const voiceIsolationWeight = source.voiceIsolationWeight ?? 1;
  const noiseGateEnabled = source.noiseGateEnabled ?? true;
  const autoNoiseGate = source.autoNoiseGate ?? true;
  const noiseGateThreshold = source.noiseGateThreshold ?? 0.05;
  const noiseGateDebounceMs = source.noiseGateDebounceMs ?? 180;
  const micDeviceId = source.micDeviceId ?? '';
  const visibleInputDevices = [...inputDevices];
  if (micDeviceId && !visibleInputDevices.some((device) => device.id === micDeviceId)) {
    visibleInputDevices.unshift({
      id: micDeviceId,
      name: source.micDeviceName || 'Selected microphone',
      isDefault: false,
      state: 'unavailable',
      matchKey: source.micDeviceMatchKey ?? ''
    });
  }

  return (
    <div className='modal-backdrop' onClick={onClose}>
      <div className='modal' onClick={(event) => event.stopPropagation()}>
        <div className='modal-header'>
          <h2>Microphone Settings</h2>
          <button className='icon-button' onClick={onClose}><X size={18} /></button>
        </div>
        <div className='modal-body microphone-modal-body'>
          <label>
            Input device
            <select
              value={micDeviceId}
              onChange={(event) => {
                const selectedId = event.target.value;
                const selectedDevice = visibleInputDevices.find((device) => device.id === selectedId);
                onUpdate({
                  micDeviceId: selectedId,
                  micDeviceMatchKey: selectedDevice?.matchKey ?? '',
                  micDeviceName: selectedDevice?.name ?? ''
                });
              }}
            >
              <option value=''>System default</option>
              {visibleInputDevices.map((device) => (
                <option key={device.id} value={device.id}>
                  {device.name}{device.state === 'unavailable' ? ' (unplugged, using default)' : device.isDefault ? ' (default)' : ''}
                </option>
              ))}
            </select>
          </label>

          <label>
            Mic gain
            <div className='range-line'>
              <input
                type='range'
                min={minMicGainDb}
                max={maxMicGainDb}
                step={0.5}
                value={volumeDb}
                onChange={(event) => onUpdate({ volume: dbToGain(Number(event.target.value)) })}
              />
              <span>{formatDb(volumeDb)}</span>
            </div>
          </label>

          <label>
            Voice isolation
            <select value={voiceIsolation ? 'on' : 'off'} onChange={(event) => onUpdate({ voiceIsolation: event.target.value === 'on' })}>
              <option value='off'>Off</option>
              <option value='on'>On</option>
            </select>
          </label>

          {voiceIsolation && (
            <label>
              Isolation strength
              <div className='range-line'>
                <input
                  type='range'
                  min={0}
                  max={1}
                  step={0.05}
                  value={voiceIsolationWeight}
                  onChange={(event) => onUpdate({ voiceIsolationWeight: Number(event.target.value) })}
                />
                <span>{Math.round(voiceIsolationWeight * 100)}%</span>
              </div>
            </label>
          )}

          <label>
            Input sensitivity
            <select
              value={!noiseGateEnabled ? 'off' : autoNoiseGate ? 'auto' : 'manual'}
              onChange={(event) => {
                const mode = event.target.value;
                onUpdate({ noiseGateEnabled: mode !== 'off', autoNoiseGate: mode === 'auto', noiseGateThreshold });
              }}
            >
              <option value='off'>Off</option>
              <option value='auto'>Auto</option>
              <option value='manual'>Manual</option>
            </select>
          </label>

          {noiseGateEnabled && !autoNoiseGate && (
            <label>
              Manual sensitivity
              <div className='range-line'>
                <input
                  type='range'
                  min={0}
                  max={100}
                  step={0.5}
                  value={sensitivityFromThreshold(noiseGateThreshold)}
                  onChange={(event) => onUpdate({ noiseGateThreshold: thresholdFromSensitivity(Number(event.target.value)) })}
                />
                <span>{sensitivityFromThreshold(noiseGateThreshold)}%</span>
              </div>
            </label>
          )}

          {noiseGateEnabled && (
            <label>
              Debounce time
              <div className='range-line'>
                <input
                  type='range'
                  min={0}
                  max={1000}
                  step={20}
                  value={noiseGateDebounceMs}
                  onChange={(event) => onUpdate({ noiseGateDebounceMs: Number(event.target.value) })}
                />
                <span>{noiseGateDebounceMs}ms</span>
              </div>
            </label>
          )}

          <div className='mic-test-row'>
            <TestMicButton
              volume={volume}
              voiceIsolation={voiceIsolation}
              voiceIsolationWeight={voiceIsolationWeight}
              noiseGateEnabled={noiseGateEnabled}
              autoNoiseGate={autoNoiseGate}
              noiseGateThreshold={noiseGateThreshold}
              noiseGateDebounceMs={noiseGateDebounceMs}
            />
          </div>
        </div>
      </div>
    </div>
  );
}
