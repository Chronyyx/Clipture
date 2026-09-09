import { Maximize2, Pause, Play, Volume2, VolumeX } from 'lucide-react';
import { useEffect, useRef, type CSSProperties } from 'react';
import { formatDuration } from '../../shared/clips/clipMetadata';

interface PlayerControlsProps {
  currentTime: number;
  duration: number;
  playing: boolean;
  volume: number;
  onSeek: (percent: number) => void;
  onTogglePlayback: () => void;
  onToggleFullscreen: () => void;
  onVolumeChange: (volume: number) => void;
}

export function PlayerControls({
  currentTime,
  duration,
  playing,
  volume,
  onSeek,
  onTogglePlayback,
  onToggleFullscreen,
  onVolumeChange
}: PlayerControlsProps) {
  const previousVolume = useRef(volume > 0 ? volume : 1);
  const muted = volume === 0;
  useEffect(() => {
    if (volume > 0) previousVolume.current = volume;
  }, [volume]);

  function toggleMute() {
    if (muted) {
      onVolumeChange(previousVolume.current);
    } else {
      previousVolume.current = volume;
      onVolumeChange(0);
    }
  }

  const progress = duration > 0 ? Math.min(100, Math.max(0, currentTime / duration * 100)) : 0;
  return (
    <div className='player-controls field-transport'>
      <div className='yt-pill transport-playback'>
        <button className='icon-button transport-play' title={playing ? 'Pause' : 'Play'} aria-label={playing ? 'Pause' : 'Play'} onClick={onTogglePlayback}>
          <span className={`transport-led${playing ? ' active' : ''}`} aria-hidden='true' />
          {playing ? <Pause size={24} /> : <Play size={24} />}
        </button>
        <span className='time-readout transport-total' title='Clip duration'>{formatDuration(duration)}</span>
      </div>
      <div className='volume-container'>
        <button className='transport-mute' type='button' onClick={toggleMute} title={muted ? 'Unmute' : 'Mute'} aria-label='Mute audio' aria-pressed={muted}>
          {muted ? <VolumeX size={16} aria-hidden='true' /> : <Volume2 size={16} aria-hidden='true' />}
        </button>
        <input className='volume-slider' type='range' min={0} max={5.62} step={0.05} value={volume} onChange={(event) => onVolumeChange(Number(event.target.value))} aria-label='Volume' />
      </div>
      <div className='transport-timeline'>
        <span className='transport-time'>{formatDuration(currentTime)}</span>
        <div className='transport-scale' style={{ '--playback-progress': `${progress}%` } as CSSProperties}>
          <input className='player-scrubber' type='range' min={0} max={100} step={0.1} value={progress} onChange={(event) => onSeek(Number(event.target.value))} aria-label='Seek' aria-valuetext={`${formatDuration(currentTime)} of ${formatDuration(duration)}`} />
        </div>
        <span className='transport-time'>−{formatDuration(Math.max(0, duration - currentTime))}</span>
      </div>
      <button className='yt-pill icon-button' title='Fullscreen' aria-label='Fullscreen' onClick={onToggleFullscreen}><Maximize2 size={22} /></button>
    </div>
  );
}
