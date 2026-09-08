import { Maximize2, Pause, Play, Volume2 } from 'lucide-react';
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
  const progress = duration > 0 ? Math.min(100, Math.max(0, currentTime / duration * 100)) : 0;
  return (
    <div className='player-controls'>
      <input className='player-scrubber' type='range' min={0} max={100} step={0.1} value={progress} onChange={(event) => onSeek(Number(event.target.value))} aria-label='Seek' />
      <div className='player-controls-bottom'>
        <div className='player-controls-left'>
          <button className='yt-pill icon-button' title={playing ? 'Pause' : 'Play'} onClick={onTogglePlayback}>
            {playing ? <Pause size={24} /> : <Play size={24} />}
          </button>
          <div className='yt-pill volume-container'>
            <Volume2 size={22} />
            <input className='volume-slider' type='range' min={0} max={5.62} step={0.05} value={volume} onChange={(event) => onVolumeChange(Number(event.target.value))} aria-label='Volume' />
          </div>
          <div className='yt-pill time-readout'>{formatDuration(currentTime)} / {formatDuration(duration)}</div>
        </div>
        <div className='player-controls-right'>
          <button className='yt-pill icon-button' title='Fullscreen' onClick={onToggleFullscreen}><Maximize2 size={22} /></button>
        </div>
      </div>
    </div>
  );
}
