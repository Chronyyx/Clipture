import { X } from 'lucide-react';
import { useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import type { ClipRecord, ClipSettings } from '../../../shared/types';
import { clipture } from '../../platform';
import { clipSourceLabels, displayAudioTracks, formatDuration } from '../../shared/clips/clipMetadata';
import { useClipIconUrl } from '../../shared/clips/useClipIconUrl';
import { PlayerVideoSurface } from './PlayerVideoSurface';
import { useMixedAudioPlayback } from './useMixedAudioPlayback';
import { usePlayerInteractions } from './usePlayerInteractions';

interface ClipPlayerProps {
  clip: ClipRecord;
  onClose: () => void;
  settings?: ClipSettings;
  sidebar?: ReactNode;
}

export function ClipPlayer({
  clip,
  onClose,
  settings,
  sidebar
}: ClipPlayerProps) {
  const videoRef = useRef<HTMLVideoElement>(null);
  const playbackRequestedRef = useRef(true);
  const [sourceUrl, setSourceUrl] = useState('');
  const [message, setMessage] = useState('Preparing playback');
  const [mixedPlayback, setMixedPlayback] = useState(false);
  const [mixedAudioChunkUrl, setMixedAudioChunkUrl] = useState('');
  const [mixedAudioChunkSeconds, setMixedAudioChunkSeconds] = useState(8);
  const [playing, setPlaying] = useState(false);
  const [currentTime, setCurrentTime] = useState(0);
  const [duration, setDuration] = useState(clip.durationSeconds);
  const [volume, setVolume] = useState(1);
  const sourceLabels = useMemo(() => clipSourceLabels(clip, settings), [clip, settings]);
  const sourceText = sourceLabels.length > 0 ? sourceLabels.join(', ') : clip.gameOrApp;
  const iconUrl = useClipIconUrl(clip, sourceLabels);
  const embedded = Boolean(sidebar);
  const displayTitle = clip.title === 'Clipture clip' ? 'Clipture' : clip.title;

  const mixed = useMixedAudioPlayback({
    videoRef,
    enabled: mixedPlayback,
    chunkUrl: mixedAudioChunkUrl,
    chunkSeconds: mixedAudioChunkSeconds,
    duration,
    sourceUrl,
    volume,
    playbackRequestedRef,
    onPlayingChange: setPlaying
  });

  const interactions = usePlayerInteractions({
    videoRef,
    duration,
    mixedEnabled: mixedPlayback,
    playbackRequestedRef,
    mixed,
    setCurrentTime,
    setPlaying
  });

  useEffect(() => {
    let active = true;
    setSourceUrl('');
    setMessage('Preparing playback');
    setMixedPlayback(false);
    setMixedAudioChunkUrl('');
    setMixedAudioChunkSeconds(8);
    setPlaying(false);
    setCurrentTime(0);
    setDuration(clip.durationSeconds);
    playbackRequestedRef.current = true;
    mixed.cancelPlayRequest();
    mixed.clear();

    void clipture.clipPlaybackUrl(clip.filePath, clip.audioTracks).then((result) => {
      if (!active) return;
      setSourceUrl(result.url);
      setMessage(result.message);
      setMixedPlayback(result.mixed && Boolean(result.audioChunkUrl));
      setMixedAudioChunkUrl(result.audioChunkUrl || '');
      setMixedAudioChunkSeconds(result.audioChunkSeconds || 8);
    }).catch((error) => {
      if (active) setMessage(error instanceof Error ? error.message : 'Could not prepare playback.');
    });

    return () => {
      active = false;
      void clipture.releasePlaybackCache().catch(error => console.warn('Playback release failed:', error));
    };
  }, [clip.audioTracks, clip.filePath]);

  const [aspectWidth, aspectHeight] = (clip.resolution || '').split('x').map((part) => Number.parseInt(part));

  return (
    <section className={embedded ? 'player panel library-player' : 'player panel'}>
      <div className={embedded ? 'library-player-main' : 'player-content'}>
        {!embedded && (
          <div className='player-header'>
            <div>
              <strong className='player-title'>
                {iconUrl && <img className='clip-app-icon' src={iconUrl} alt='' />}
                <span>{displayTitle}</span>
              </strong>
              <span>{sourceText}</span>
            </div>
            <button className='icon-button' title='Close player' onClick={onClose}><X size={16} /></button>
          </div>
        )}
        {sourceUrl ? (
          <PlayerVideoSurface
            videoRef={videoRef}
            sourceUrl={sourceUrl}
            embedded={embedded}
            aspectWidth={aspectWidth || 16}
            aspectHeight={aspectHeight || 9}
            mixedEnabled={mixedPlayback}
            mixed={mixed}
            interactions={interactions}
            playbackRequestedRef={playbackRequestedRef}
            playing={playing}
            currentTime={currentTime}
            duration={duration}
            volume={volume}
            setPlaying={setPlaying}
            setCurrentTime={setCurrentTime}
            setDuration={setDuration}
            setVolume={setVolume}
          />
        ) : <div className='empty'>{message}</div>}
        {!embedded && (
          <div className='player-meta'>
            <span>{formatDuration(clip.durationSeconds)}</span>
            <span>{clip.resolution}</span>
            <span>{clip.fps} FPS</span>
            <span>{displayAudioTracks(clip.audioTracks) || 'No audio tracks'}</span>
            <span>{message}</span>
          </div>
        )}
      </div>
      {sidebar}
    </section>
  );
}
