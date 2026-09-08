import { useEffect, useState } from 'react';
import type { ClipSettings, DisplayDevice } from '../../../shared/types';
import { clipture } from '../../platform';
import { DraftNumberInput } from './DraftNumberInput';

interface VideoSettingsProps {
  settings: ClipSettings;
  onChange: (patch: Partial<ClipSettings>) => void;
}

export function VideoSettings({ settings, onChange }: VideoSettingsProps) {
  const [displayDevices, setDisplayDevices] = useState<DisplayDevice[]>([]);
  const [editingClipLength, setEditingClipLength] = useState(false);
  const [clipLengthDraft, setClipLengthDraft] = useState(() => String(settings.clipLengthSeconds));

  useEffect(() => {
    void clipture.listDisplayDevices().then(setDisplayDevices).catch(() => setDisplayDevices([]));
  }, []);

  useEffect(() => {
    if (!editingClipLength) setClipLengthDraft(String(settings.clipLengthSeconds));
  }, [editingClipLength, settings.clipLengthSeconds]);

  function commitClipLengthDraft() {
    const parsed = Number(clipLengthDraft.trim());
    if (!clipLengthDraft.trim() || !Number.isFinite(parsed)) {
      setClipLengthDraft(String(settings.clipLengthSeconds));
      return;
    }
    const nextLength = Math.min(600, Math.max(5, Math.round(parsed)));
    setClipLengthDraft(String(nextLength));
    if (nextLength !== settings.clipLengthSeconds) onChange({ clipLengthSeconds: nextLength });
  }

  return (
    <div className='settings-group'>
      <label>
        Display
        <select value={settings.monitorId || 'primary'} onChange={(event) => onChange({ monitorId: event.target.value })}>
          <option value='primary'>Primary display</option>
          {displayDevices.map((display) => (
            <option key={display.id} value={display.id}>
              {display.name} ({display.width}x{display.height}){display.isPrimary ? ' primary' : ''}{display.hdr ? ' HDR' : ''}
            </option>
          ))}
        </select>
      </label>
      <label>
        Clip length
        <input
          type='text'
          inputMode='numeric'
          pattern='[0-9]*'
          value={clipLengthDraft}
          onFocus={() => setEditingClipLength(true)}
          onChange={(event) => {
            if (/^\d*$/.test(event.target.value)) setClipLengthDraft(event.target.value);
          }}
          onBlur={() => {
            setEditingClipLength(false);
            commitClipLengthDraft();
          }}
          onKeyDown={(event) => {
            if (event.key === 'Enter') event.currentTarget.blur();
            if (event.key === 'Escape') {
              setClipLengthDraft(String(settings.clipLengthSeconds));
              event.currentTarget.blur();
            }
          }}
        />
      </label>
      <label>
        FPS
        <select value={settings.fps} onChange={(event) => onChange({ fps: Number(event.target.value) as ClipSettings['fps'] })}>
          <option value={24}>24 low resource</option>
          <option value={30}>30 default</option>
          <option value={60}>60 high motion</option>
        </select>
      </label>
      <label>
        Resolution
        <select value={settings.resolutionPreset} onChange={(event) => onChange({ resolutionPreset: event.target.value as ClipSettings['resolutionPreset'] })}>
          <option value='system'>System resolution</option>
          <option value='144p'>144p</option>
          <option value='360p'>360p</option>
          <option value='720p'>720p</option>
          <option value='1080p'>1080p</option>
          <option value='1440p'>1440p</option>
          <option value='4k'>4K</option>
        </select>
      </label>
      <label>
        Bitrate Mbps
        <DraftNumberInput min={4} max={120} value={settings.bitrateMbps} disabled={settings.autoBitrate} onCommit={(bitrateMbps) => onChange({ bitrateMbps })} />
      </label>
      <label className='toggle-label'>
        <input className='toggle-switch' type='checkbox' checked={settings.autoBitrate} onChange={(event) => onChange({ autoBitrate: event.target.checked })} />
        Auto bitrate
      </label>
      <label>
        Max auto bitrate Mbps
        <DraftNumberInput min={4} max={120} value={settings.maxAutoBitrateMbps} disabled={!settings.autoBitrate} onCommit={(maxAutoBitrateMbps) => onChange({ maxAutoBitrateMbps })} />
      </label>
      <label>
        NVENC preset
        <select value={settings.nvencPreset} onChange={(event) => onChange({ nvencPreset: Number(event.target.value) as ClipSettings['nvencPreset'] })}>
          <option value={1}>P1 fastest</option>
          <option value={2}>P2 low resource</option>
          <option value={3}>P3 balanced</option>
          <option value={4}>P4 quality</option>
          <option value={5}>P5 higher quality</option>
        </select>
      </label>
    </div>
  );
}
