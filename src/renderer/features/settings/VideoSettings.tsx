import { useEffect, useState } from 'react';
import type { ClipSettings, DisplayDevice } from '../../../shared/types';
import { CAPTURE_FPS_OPTIONS } from '../../../shared/capture-fps';
import { clipture } from '../../platform';
import { DraftNumberInput } from './DraftNumberInput';
import { SettingRow, SettingsSection, Toggle } from './SettingsSection';

interface VideoSettingsProps {
  settings: ClipSettings;
  onChange: (patch: Partial<ClipSettings>) => void;
}

const resolutions: Array<[ClipSettings['resolutionPreset'], string]> = [
  ['system', 'Same as display'],
  ['4k', '2160p (4K)'],
  ['1440p', '1440p'],
  ['1080p', '1080p'],
  ['720p', '720p'],
  ['360p', '360p'],
  ['144p', '144p']
];

const presets: Array<[ClipSettings['nvencPreset'], string]> = [
  [1, 'Fastest'],
  [2, 'Light'],
  [3, 'Balanced'],
  [4, 'Quality'],
  [5, 'Best quality']
];

export function VideoSettings({ settings, onChange }: VideoSettingsProps) {
  const [displays, setDisplays] = useState<DisplayDevice[]>();

  useEffect(() => {
    let active = true;
    void clipture.listDisplayDevices()
      .then((next) => { if (active) setDisplays(next); })
      .catch(() => { if (active) setDisplays([]); });
    return () => { active = false; };
  }, []);

  return (
    <SettingsSection title='Video' description='Picture quality and how much work the GPU does while you play.'>
      <SettingRow
        title='Display'
        description='The screen Clipture records.'
        control={(id) => displays === undefined ? <span className='skeleton control-skeleton' aria-label='Loading displays' /> : (
          <select id={id} value={settings.monitorId || 'primary'} onChange={(event) => onChange({ monitorId: event.target.value })}>
            <option value='primary'>Primary display</option>
            {displays.map((display) => (
              <option key={display.id} value={display.id}>
                {display.name} ({display.width}×{display.height}{display.hdr ? ', HDR' : ''})
              </option>
            ))}
          </select>
        )}
      />
      <SettingRow
        title='Frame rate'
        description='60 FPS suits most games. Above 60 is experimental and depends on your GPU and display.'
        control={(id) => (
          <select id={id} value={settings.fps} onChange={(event) => onChange({ fps: Number(event.target.value) as ClipSettings['fps'] })}>
            {CAPTURE_FPS_OPTIONS.map((fps) => <option key={fps} value={fps}>{fps} FPS{fps > 60 ? ' (experimental)' : ''}</option>)}
          </select>
        )}
      />
      <SettingRow
        title='Resolution'
        description='Clips are scaled to this size. Lower sizes make smaller files.'
        control={(id) => (
          <select id={id} value={settings.resolutionPreset} onChange={(event) => onChange({ resolutionPreset: event.target.value as ClipSettings['resolutionPreset'] })}>
            {resolutions.map(([value, label]) => <option key={value} value={value}>{label}</option>)}
          </select>
        )}
      />
      <SettingRow
        title='Automatic bitrate'
        description='Picks a bitrate limit from the resolution and frame rate.'
        control={(id) => <Toggle id={id} checked={settings.autoBitrate} onChange={(autoBitrate) => onChange({ autoBitrate })} />}
      />
      {settings.autoBitrate ? (
        <SettingRow
          title='Maximum bitrate'
          description='Fast action can use up to this; quieter scenes use less. Higher keeps fast action sharper.'
          control={(id) => (
            <span className='input-with-unit'>
              <DraftNumberInput id={id} min={4} max={120} value={settings.maxAutoBitrateMbps} onCommit={(maxAutoBitrateMbps) => onChange({ maxAutoBitrateMbps })} />
              <span aria-hidden='true'>Mbps</span>
            </span>
          )}
        />
      ) : (
        <SettingRow
          title='Maximum bitrate'
          description='Fast action can use up to this; quieter scenes use less. 40 Mbps suits 1080p at 60 FPS.'
          control={(id) => (
            <span className='input-with-unit'>
              <DraftNumberInput id={id} min={4} max={120} value={settings.bitrateMbps} onCommit={(bitrateMbps) => onChange({ bitrateMbps })} />
              <span aria-hidden='true'>Mbps</span>
            </span>
          )}
        />
      )}
      <SettingRow
        title='Encoder preset'
        description='Faster presets leave more GPU for your game. Balanced suits most PCs.'
        control={(id) => (
          <select id={id} value={settings.nvencPreset} onChange={(event) => onChange({ nvencPreset: Number(event.target.value) as ClipSettings['nvencPreset'] })}>
            {presets.map(([value, label]) => <option key={value} value={value}>{label}</option>)}
          </select>
        )}
      />
    </SettingsSection>
  );
}
