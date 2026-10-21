import { Maximize2, Pause, Play, Scissors, SlidersHorizontal, Volume2, VolumeX } from 'lucide-react';
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
  /** Shown only for clips that can be trimmed. */
  onToggleTrim?: () => void;
  trimming?: boolean;
  /** Shown only for clips whose audio can be remixed. */
  onToggleMixer?: () => void;
  mixing?: boolean;
}

export function PlayerControls({
  currentTime,
  duration,
  playing,
  volume,
  onSeek,
  onTogglePlayback,
  onToggleFullscreen,
  onVolumeChange,
  onToggleTrim,
  trimming = false,
  onToggleMixer,
  mixing = false
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
          {playing ? <Pause size={24} /> : <Play size={24} />}
        </button>
        <span className='time-readout transport-position' title='Position'>{formatDuration(currentTime)}</span>
      </div>
      <div className='volume-container'>
        <button className='transport-mute' type='button' onClick={toggleMute} title={muted ? 'Unmute' : 'Mute'} aria-label='Mute audio' aria-pressed={muted}>
          {muted ? <VolumeX size={16} aria-hidden='true' /> : <Volume2 size={16} aria-hidden='true' />}
        </button>
        <input className='volume-slider' type='range' min={0} max={5.62} step={0.05} value={volume} onChange={(event) => onVolumeChange(Number(event.target.value))} aria-label='Volume' />
      </div>
      <div className='transport-timeline'>
        <div className='transport-scale' style={{ '--playback-progress': `${progress}%` } as CSSProperties}>
          <input className='player-scrubber' type='range' min={0} max={100} step={0.1} value={progress} onChange={(event) => onSeek(Number(event.target.value))} aria-label='Seek' aria-valuetext={`${formatDuration(currentTime)} of ${formatDuration(duration)}`} />
        </div>
        <span className='transport-time' title='Clip length'>{formatDuration(duration)}</span>
      </div>
      {onToggleMixer && (
        <button className={`yt-pill icon-button${mixing ? ' active' : ''}`} title={mixing ? 'Close the audio mixer' : 'Balance the audio tracks'}
          aria-label='Audio mixer' aria-pressed={mixing} onClick={onToggleMixer}><SlidersHorizontal size={22} /></button>
      )}
      {onToggleTrim && (
        <button className={`yt-pill icon-button${trimming ? ' active' : ''}`} title={trimming ? 'Close the trim editor' : 'Trim this clip'}
          aria-label='Trim' aria-pressed={trimming} onClick={onToggleTrim}><Scissors size={22} /></button>
      )}
      <button className='yt-pill icon-button' title='Fullscreen' aria-label='Fullscreen' onClick={onToggleFullscreen}><Maximize2 size={22} /></button>
    </div>
  );
}
