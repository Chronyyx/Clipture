import { useEffect, useRef, useState } from 'react';
import { PlayerVideoSurface } from './PlayerVideoSurface';
import { useMixedAudioPlayback } from './useMixedAudioPlayback';
import { usePlayerInteractions } from './usePlayerInteractions';

interface MediaPlayerProps {
  sourceUrl: string;
  /** Mixed audio of every track; the video element alone plays only the first. */
  audioChunkUrl?: string;
  /** Use the mixed audio. May turn on mid-playback (a stream finishing). */
  mixedAudio: boolean;
  durationHint: number;
  aspectWidth: number;
  aspectHeight: number;
  /** The video element, for callers that watch its events. */
  onVideo?: (video: HTMLVideoElement | null) => void;
}

/** Clipture's player (controls, gestures, multi-track audio) for any media
 * URL, such as a clip a friend is streaming. */
export function MediaPlayer({
  sourceUrl,
  audioChunkUrl = '',
  mixedAudio,
  durationHint,
  aspectWidth,
  aspectHeight,
  onVideo
}: MediaPlayerProps) {
  const videoRef = useRef<HTMLVideoElement>(null);
  const playbackRequestedRef = useRef(true);
  const [playing, setPlaying] = useState(false);
  const [currentTime, setCurrentTime] = useState(0);
  const [duration, setDuration] = useState(durationHint);
  const [volume, setVolume] = useState(1);
  const mixedEnabled = mixedAudio && Boolean(audioChunkUrl);

  const mixed = useMixedAudioPlayback({
    videoRef,
    enabled: mixedEnabled,
    chunkUrl: audioChunkUrl,
    chunkSeconds: 8,
    duration,
    sourceUrl,
    volume,
    playbackRequestedRef,
    onPlayingChange: setPlaying
  });

  const interactions = usePlayerInteractions({
    videoRef,
    duration,
    mixedEnabled,
    playbackRequestedRef,
    mixed,
    setCurrentTime,
    setPlaying
  });

  useEffect(() => {
    onVideo?.(videoRef.current);
    return () => onVideo?.(null);
  }, [sourceUrl]);

  // Switching to mixed audio while playing: hand over at the playhead.
  useEffect(() => {
    const video = videoRef.current;
    if (mixedEnabled && video && !video.paused) void mixed.playWhenReady();
  }, [mixedEnabled]);

  return (
    <PlayerVideoSurface
      videoRef={videoRef}
      sourceUrl={sourceUrl}
      embedded
      aspectWidth={aspectWidth}
      aspectHeight={aspectHeight}
      mixedEnabled={mixedEnabled}
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
  );
}
