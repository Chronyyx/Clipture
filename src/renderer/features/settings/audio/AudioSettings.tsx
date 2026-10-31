import { useEffect, useState } from 'react';
import { Gamepad2, Mic } from 'lucide-react';
import type { AudioInputDevice, AudioSourceRule, ClipSettings } from '../../../../shared/types';
import { clipture } from '../../../platform';
import { MicrophoneSettingsModal } from './MicrophoneSettingsModal';

interface AudioSettingsProps {
  settings: ClipSettings;
  onChange: (patch: Partial<ClipSettings>) => void;
}

function microphoneDescription(source: AudioSourceRule) {
  return source.micDeviceName ? 'Record from ' + source.micDeviceName : 'Record from your microphone';
}

// The game and every app playing sound are recorded automatically, each on its
// own track (ADR 0028); only the microphone is a choice.
export function AudioSettings({ settings, onChange }: AudioSettingsProps) {
  const audioSources = settings.audioSources || [];
  const microphone = audioSources.find((source) => source.kind === 'microphone');
  const [inputDevices, setInputDevices] = useState<AudioInputDevice[]>([]);
  const [devicesLoading, setDevicesLoading] = useState(true);
  const [configuringMic, setConfiguringMic] = useState(false);

  function refreshInputDevices() {
    setDevicesLoading(true);
    void clipture.listAudioInputDevices().then(setInputDevices).catch(() => setInputDevices([]))
      .finally(() => setDevicesLoading(false));
  }

  useEffect(() => {
    refreshInputDevices();
  }, []);

  function updateSource(sourceId: string, patch: Partial<AudioSourceRule>) {
    onChange({
      audioSources: audioSources.map((source) => source.id === sourceId ? { ...source, ...patch } : source)
    });
  }

  useEffect(() => {
    if (!microphone?.micDeviceId) return;
    const activeDevice = inputDevices.find((device) => device.id === microphone.micDeviceId);
    if (!activeDevice) return;
    const matchKey = activeDevice.matchKey ?? '';
    if (microphone.micDeviceMatchKey === matchKey && microphone.micDeviceName === activeDevice.name) return;
    updateSource(microphone.id, { micDeviceMatchKey: matchKey, micDeviceName: activeDevice.name });
  }, [inputDevices, audioSources]);

  return (
    <div className='settings-group single-column'>
      <div className='audio-settings-panel'>
        <div className='audio-settings-header'>
          <div className='audio-settings-heading'>
            <h2>Audio</h2>
            <p>Your game and every app playing sound each get their own track, so you can mute any of them later.</p>
          </div>
        </div>

        <div className='audio-source-card'>
          <div className='audio-source-row'>
            <div className='audio-source-main'>
              <span className='audio-source-icon' aria-hidden='true'><Gamepad2 size={20} /></span>
              <span className='audio-source-text'>
                <strong>Game and apps</strong>
                <span>Records your game and every app playing sound, each on its own track</span>
              </span>
            </div>
            <div className='audio-source-actions'>
              <span className='audio-status-badge'>Automatic</span>
            </div>
          </div>
          {microphone && (
            <div className='audio-source-row'>
              <div className='audio-source-main'>
                <span className='audio-source-icon' aria-hidden='true'><Mic size={20} /></span>
                <span className='audio-source-text'><strong>{microphone.label}</strong><span>{microphoneDescription(microphone)}</span></span>
              </div>
              <div className='audio-source-actions'>
                <label className='audio-source-toggle' title={microphone.enabled ? 'Disable the microphone' : 'Enable the microphone'}>
                  <input className='toggle-switch' type='checkbox' checked={microphone.enabled} onChange={(event) => updateSource(microphone.id, { enabled: event.target.checked })} />
                </label>
                <button className='secondary-button compact-configure' type='button' onClick={() => { refreshInputDevices(); setConfiguringMic(true); }}>
                  Configure
                </button>
              </div>
            </div>
          )}
          {microphone && configuringMic && (
            <MicrophoneSettingsModal
              source={microphone}
              inputDevices={inputDevices}
              loading={devicesLoading && inputDevices.length === 0}
              onUpdate={(patch) => updateSource(microphone.id, patch)}
              onClose={() => setConfiguringMic(false)}
            />
          )}
        </div>
      </div>
    </div>
  );
}
