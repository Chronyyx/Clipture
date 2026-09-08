import { Fragment, useEffect, useMemo, useState } from 'react';
import { Gamepad2, Mic, Plus, Trash2, Volume2 } from 'lucide-react';
import type { ActiveProcess, AudioInputDevice, AudioSourceRule, ClipSettings } from '../../../../shared/types';
import { clipture } from '../../../platform';
import { AppAudioModal } from './AppAudioModal';
import { AppAudioSourceIcon } from './AppAudioSourceIcon';
import { MicrophoneSettingsModal } from './MicrophoneSettingsModal';
import { SystemAudioModal } from './SystemAudioModal';

interface AudioSettingsProps {
  settings: ClipSettings;
  onChange: (patch: Partial<ClipSettings>) => void;
}

function sourceIcon(source: AudioSourceRule) {
  if (source.kind === 'microphone') return <Mic size={20} />;
  if (source.kind === 'game') return <Gamepad2 size={20} />;
  return <Volume2 size={20} />;
}

function sourceDescription(source: AudioSourceRule) {
  if (source.kind === 'system') {
    return source.captureAllSystem === false ? 'Record selected apps into the system mix' : 'Record all system sounds';
  }
  if (source.kind === 'microphone') {
    return source.micDeviceName ? 'Record from ' + source.micDeviceName : 'Record from your microphone';
  }
  if (source.kind === 'game') return 'Record audio from the active game or app';
  return source.enabled ? 'Recorded when enabled' : 'Currently disabled';
}

function appSourceName(source: AudioSourceRule) {
  return source.processName || source.label || 'Select process';
}

function appSourceInitial(source: AudioSourceRule) {
  const name = appSourceName(source).replace(/\.exe$/i, '').trim();
  return name ? name[0]!.toUpperCase() : 'A';
}

export function AudioSettings({ settings, onChange }: AudioSettingsProps) {
  const audioSources = settings.audioSources || [];
  const builtInSources = audioSources.filter((source) => source.kind !== 'app');
  const appSources = audioSources.filter((source) => source.kind === 'app');
  const [activeProcesses, setActiveProcesses] = useState<ActiveProcess[]>([]);
  const [inputDevices, setInputDevices] = useState<AudioInputDevice[]>([]);
  const [configuringSystem, setConfiguringSystem] = useState(false);
  const [configuringApp, setConfiguringApp] = useState<string | null>(null);
  const [configuringMic, setConfiguringMic] = useState<string | null>(null);
  const otherAppProcesses = useMemo(() => new Set(
    appSources.filter((source) => source.enabled && source.processName).map((source) => source.processName!)
  ), [appSources]);

  function refreshProcesses() {
    void clipture.listActiveProcesses().then(setActiveProcesses).catch(() => setActiveProcesses([]));
  }

  function refreshInputDevices() {
    void clipture.listAudioInputDevices().then(setInputDevices).catch(() => setInputDevices([]));
  }

  useEffect(() => {
    refreshProcesses();
    refreshInputDevices();
  }, []);

  function updateSource(sourceId: string, patch: Partial<AudioSourceRule>) {
    onChange({
      audioSources: audioSources.map((source) => source.id === sourceId ? { ...source, ...patch } : source)
    });
  }

  useEffect(() => {
    const microphone = audioSources.find((source) => source.kind === 'microphone');
    if (!microphone?.micDeviceId) return;
    const activeDevice = inputDevices.find((device) => device.id === microphone.micDeviceId);
    if (!activeDevice) return;
    const matchKey = activeDevice.matchKey ?? '';
    if (microphone.micDeviceMatchKey === matchKey && microphone.micDeviceName === activeDevice.name) return;
    updateSource(microphone.id, { micDeviceMatchKey: matchKey, micDeviceName: activeDevice.name });
  }, [inputDevices, audioSources]);

  function addSource() {
    const id = 'app-' + Date.now();
    onChange({
      audioSources: [...audioSources, { id, kind: 'app', label: 'New App Source', enabled: true, omitIfSilent: true }]
    });
    refreshProcesses();
    setConfiguringApp(id);
  }

  function removeSource(sourceId: string) {
    const source = audioSources.find((candidate) => candidate.id === sourceId);
    if (!source || source.kind !== 'app') return;
    if (configuringApp === sourceId) setConfiguringApp(null);
    onChange({ audioSources: audioSources.filter((candidate) => candidate.id !== sourceId) });
  }

  return (
    <div className='settings-group single-column'>
      <div className='audio-settings-panel'>
        <div className='audio-settings-header'>
          <div className='audio-settings-heading'>
            <h2>Audio sources</h2>
            <p>Choose what audio to record and how it is captured.</p>
          </div>
          <button className='add-source wide-add-source' title='Add audio source' type='button' onClick={addSource}>
            <Plus size={16} /><span>Add source</span>
          </button>
        </div>

        <div className='audio-source-card'>
          {builtInSources.map((source) => (
            <Fragment key={source.id}>
              <div className='audio-source-row'>
                <div className='audio-source-main'>
                  <span className='audio-source-icon' aria-hidden='true'>{sourceIcon(source)}</span>
                  <span className='audio-source-text'><strong>{source.label}</strong><span>{sourceDescription(source)}</span></span>
                </div>
                <div className='audio-source-actions'>
                  <label className='audio-source-toggle' title={source.enabled ? 'Disable this source' : 'Enable this source'}>
                    <input className='toggle-switch' type='checkbox' checked={source.enabled} onChange={(event) => updateSource(source.id, { enabled: event.target.checked })} />
                  </label>
                  {source.kind === 'system' && (
                    <button className='secondary-button compact-configure' type='button' onClick={() => { refreshProcesses(); setConfiguringSystem(true); }}>
                      Configure
                    </button>
                  )}
                  {source.kind === 'microphone' && (
                    <button className='secondary-button compact-configure' type='button' onClick={() => { refreshInputDevices(); setConfiguringMic(source.id); }}>
                      Configure
                    </button>
                  )}
                  {source.kind === 'game' && <span className='audio-status-badge'>Auto-detected</span>}
                </div>
              </div>
              {source.kind === 'system' && configuringSystem && (
                <SystemAudioModal
                  source={source}
                  activeProcesses={activeProcesses}
                  otherAppProcesses={otherAppProcesses}
                  onSave={(patch) => updateSource(source.id, patch)}
                  onClose={() => setConfiguringSystem(false)}
                />
              )}
              {source.kind === 'microphone' && configuringMic === source.id && (
                <MicrophoneSettingsModal
                  source={source}
                  inputDevices={inputDevices}
                  onUpdate={(patch) => updateSource(source.id, patch)}
                  onClose={() => setConfiguringMic(null)}
                />
              )}
            </Fragment>
          ))}
        </div>

        <section className='separate-app-section'>
          <div className='audio-section-copy'>
            <h3>Separate app tracks</h3>
            <p>Apps you allow will be recorded on their own separate tracks. You can remove them anytime.</p>
          </div>
          <div className='audio-source-card app-track-card'>
            {appSources.length === 0 ? <div className='audio-source-empty'>No separate app tracks yet.</div> : appSources.map((source) => (
              <Fragment key={source.id}>
                <div
                  className='audio-source-row app-track-row'
                  onAuxClick={(event) => {
                    if (event.button !== 1) return;
                    event.preventDefault();
                    removeSource(source.id);
                  }}
                >
                  <button className='audio-source-main audio-source-main-button' type='button' onClick={() => { refreshProcesses(); setConfiguringApp(source.id); }}>
                    <AppAudioSourceIcon source={source} fallback={appSourceInitial(source)} />
                    <span className='audio-source-text'><strong>{appSourceName(source)}</strong><span>Separate track</span></span>
                  </button>
                  <div className='audio-source-actions'>
                    <label className='audio-source-toggle' title={source.enabled ? 'Disable this track' : 'Enable this track'}>
                      <input className='toggle-switch' type='checkbox' checked={source.enabled} onChange={(event) => updateSource(source.id, { enabled: event.target.checked })} />
                    </label>
                    <button aria-label={'Delete ' + appSourceName(source)} className='audio-icon-button danger' onClick={() => removeSource(source.id)} title='Delete audio source' type='button'>
                      <Trash2 size={17} />
                    </button>
                  </div>
                </div>
                {configuringApp === source.id && (
                  <AppAudioModal
                    source={source}
                    activeProcesses={activeProcesses}
                    onSave={(patch) => { updateSource(source.id, patch); setConfiguringApp(null); }}
                    onClose={() => setConfiguringApp(null)}
                  />
                )}
              </Fragment>
            ))}
          </div>
        </section>
      </div>
    </div>
  );
}
