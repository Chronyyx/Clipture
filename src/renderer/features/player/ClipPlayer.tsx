import { Check, X } from 'lucide-react';
import { useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import type { ClipRecord, ClipSettings } from '../../../shared/types';
import { clipture } from '../../platform';
import { clipSourceLabels, displayAudioTracks, formatDuration } from '../../shared/clips/clipMetadata';
import { useClipIconUrl } from '../../shared/clips/useClipIconUrl';
import { AudioMixer } from './AudioMixer';
import { PlayerVideoSurface } from './PlayerVideoSurface';
import { TrimEditor } from './TrimEditor';
import { useClipWaveform } from './clipWaveform';
import { useClipMix } from './useClipMix';
import { useClipTrim } from './useClipTrim';
import { useMixedAudioPlayback } from './useMixedAudioPlayback';
import { usePlayerInteractions } from './usePlayerInteractions';
import { preparePlayback } from './preparePlayback';

// Releasing playback frees every session this window owns. Opening the next
// clip must wait for that release, or the release can land after the open and
// kill the new session, leaving a black player until the clip is reopened.
let pendingRelease: Promise<unknown> = Promise.resolve();

/** The tracks playback mixes, when the host does not say (older hosts). */
const playedTracks = (tracks: string[]) =>
  tracks.flatMap((track, index) => (track === 'mixed-preview-pcm' || track === 'Mixed preview' ? [] : [index]));

/** The same chunks with every track apart, for the mixer. */
const withTracksApart = (url: string) => (url ? `${url}${url.includes('?') ? '&' : '?'}tracks=1` : url);

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
  const [audioTrackIndexes, setAudioTrackIndexes] = useState<number[]>([]);
  // Chunks with every track apart are shorter; 0 when the host has none.
  const [trackChunkSeconds, setTrackChunkSeconds] = useState(0);
  const [waveformUrl, setWaveformUrl] = useState<string>();
  const [playing, setPlaying] = useState(false);
  const [currentTime, setCurrentTime] = useState(0);
  const [duration, setDuration] = useState(clip.durationSeconds);
  const [volume, setVolume] = useState(1);
  // Bumped after a trim or mix: the file changed under the open session.
  const [reload, setReload] = useState(0);
  const sourceLabels = useMemo(() => clipSourceLabels(clip, settings), [clip, settings]);
  const sourceText = sourceLabels.length > 0 ? sourceLabels.join(', ') : clip.gameOrApp;
  const iconUrl = useClipIconUrl(clip, sourceLabels);
  const embedded = Boolean(sidebar);
  const displayTitle = clip.title === 'Clipture clip' ? 'Clipture' : clip.title;

  const mix = useClipMix(clip, audioTrackIndexes, () => setReload((count) => count + 1));
  // A single track plays from the video element: its level scales the volume.
  const previewVolume = !mixedPlayback && mix.previewGains ? volume * (mix.previewGains[0] ?? 1) : volume;

  const tracksApart = mixedPlayback && Boolean(mix.previewGains) && trackChunkSeconds > 0;

  const mixed = useMixedAudioPlayback({
    videoRef,
    enabled: mixedPlayback,
    chunkUrl: tracksApart ? withTracksApart(mixedAudioChunkUrl) : mixedAudioChunkUrl,
    chunkSeconds: tracksApart ? trackChunkSeconds : mixedAudioChunkSeconds,
    duration,
    sourceUrl,
    volume: previewVolume,
    playbackRequestedRef,
    onPlayingChange: setPlaying,
    trackGains: tracksApart ? mix.previewGains : undefined
  });

  const trim = useClipTrim(clip, videoRef, duration, () => setReload((count) => count + 1));
  // Fetched as the clip opens (the host builds it once and keeps it), so the
  // trim editor has it the moment it opens.
  const waveform = useClipWaveform(waveformUrl);
  // One editor at a time under the player.
  const toggleTrim = () => {
    if (!trim.range) mix.close();
    trim.toggle();
  };
  const toggleMix = () => {
    if (trim.range && !mix.open) trim.toggle();
    mix.toggle();
  };

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
    setSourceUrl('');
    setMessage('Preparing playback');
    setMixedPlayback(false);
    setMixedAudioChunkUrl('');
    setMixedAudioChunkSeconds(8);
    setAudioTrackIndexes(playedTracks(clip.audioTracks));
    setTrackChunkSeconds(0);
    setWaveformUrl(undefined);
    setPlaying(false);
    setCurrentTime(0);
    setDuration(clip.durationSeconds);
    playbackRequestedRef.current = true;
    mixed.cancelPlayRequest();
    mixed.clear();

    const cancelPreparation = preparePlayback(
      () => pendingRelease.then(() => clipture.clipPlaybackUrl(clip.filePath, clip.audioTracks)), (result) => {
      setSourceUrl(result.url);
      setMessage(result.message);
      setMixedPlayback(result.mixed && Boolean(result.audioChunkUrl));
      setMixedAudioChunkUrl(result.audioChunkUrl || '');
      setMixedAudioChunkSeconds(result.audioChunkSeconds || 8);
      if (result.audioTrackIndexes) setAudioTrackIndexes(result.audioTrackIndexes);
      setTrackChunkSeconds(result.trackChunkSeconds || 0);
      setWaveformUrl(result.waveformUrl);
    }, (error) => {
      setMessage(error instanceof Error ? error.message : 'Could not prepare playback.');
    });

    return () => {
      cancelPreparation();
      pendingRelease = clipture.releasePlaybackCache().catch(error => console.warn('Playback release failed:', error));
    };
  }, [clip.audioTracks, clip.filePath, reload]);

  const editor = !sourceUrl ? null : trim.range ? (
    <TrimEditor
      video={videoRef}
      duration={duration}
      range={trim.range}
      waveform={waveform}
      busy={trim.busy}
      error={trim.error}
      onChange={trim.change}
      onSeek={trim.seek}
      onCancel={trim.toggle}
      onApply={() => void trim.apply()}
    />
  ) : mix.open ? <AudioMixer mix={mix} peaks={tracksApart ? mixed.trackPeaks : undefined} /> : null;

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
            onToggleTrim={trim.trimmable ? toggleTrim : undefined}
            trimming={Boolean(trim.range)}
            onToggleMixer={mix.mixable ? toggleMix : undefined}
            mixing={mix.open}
          />
        ) : <div className='empty'>{message}</div>}
        {!embedded && editor}
        {(trim.notice || mix.notice) && (
          <div className='edit-notice' role='status'>
            <Check size={16} aria-hidden='true' />
            <span>{trim.notice || mix.notice}</span>
          </div>
        )}
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
      {/* In the library an open editor takes the clip rail's place, so the
          video keeps its size; the rail comes back when it closes. */}
      {embedded && editor ? <aside className='library-player-sidebar edit-rail'>{editor}</aside> : sidebar}
    </section>
  );
}
