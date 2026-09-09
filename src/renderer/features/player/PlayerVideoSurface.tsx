import type { Dispatch, MutableRefObject, RefObject, SetStateAction } from 'react';
import { useLayoutEffect } from 'react';
import { unloadVideo } from './videoLifetime';
import type { MixedAudioPlayback } from './useMixedAudioPlayback';
import type { PlayerInteractions } from './usePlayerInteractions';
import { PlayerControls } from './PlayerControls';
import { SeekFeedbackOverlay } from './SeekFeedbackOverlay';

interface PlayerVideoSurfaceProps {
  videoRef: RefObject<HTMLVideoElement>;
  sourceUrl: string;
  embedded: boolean;
  aspectWidth: number;
  aspectHeight: number;
  mixedEnabled: boolean;
  mixed: MixedAudioPlayback;
  interactions: PlayerInteractions;
  playbackRequestedRef: MutableRefObject<boolean>;
  playing: boolean;
  currentTime: number;
  duration: number;
  volume: number;
  setPlaying: Dispatch<SetStateAction<boolean>>;
  setCurrentTime: Dispatch<SetStateAction<number>>;
  setDuration: Dispatch<SetStateAction<number>>;
  setVolume: Dispatch<SetStateAction<number>>;
}

export function PlayerVideoSurface({
  videoRef,
  sourceUrl,
  embedded,
  aspectWidth,
  aspectHeight,
  mixedEnabled,
  mixed,
  interactions,
  playbackRequestedRef,
  playing,
  currentTime,
  duration,
  volume,
  setPlaying,
  setCurrentTime,
  setDuration,
  setVolume
}: PlayerVideoSurfaceProps) {
  useLayoutEffect(() => {
    // Capture the element: React clears object refs before passive cleanup.
    const video = videoRef.current;
    if (!video) return;
    playbackRequestedRef.current = true;
    const stopPlayback = () => {
      playbackRequestedRef.current = false;
      mixed.cancelPlayRequest();
      mixed.clear();
      video.pause();
    };
    const onVisibility = () => { if (document.hidden) stopPlayback(); };
    document.addEventListener('visibilitychange', onVisibility);
    return () => {
      document.removeEventListener('visibilitychange', onVisibility);
      stopPlayback();
      unloadVideo(video);
    };
  }, [sourceUrl]);

  return (
    <div
      className={'custom-video-shell ' + (interactions.controlsVisible ? '' : 'controls-hidden')}
      style={embedded ? { maxWidth: 'none' } : { maxWidth: 'calc(480px * (' + aspectWidth + ' / ' + aspectHeight + '))' }}
      onPointerMove={interactions.showControls}
      onPointerEnter={interactions.showControls}
      onPointerDown={(event) => {
        if ((event.target as HTMLElement).closest('.player-controls') || event.button !== 0) return;
        interactions.beginFastHold();
      }}
      onPointerUp={interactions.endFastHold}
      onPointerCancel={() => { interactions.endFastHold(); interactions.hideControls(); }}
      onPointerLeave={() => { interactions.endFastHold(); interactions.hideControls(); }}
    >
      <video
        ref={videoRef}
        key={sourceUrl}
        src={sourceUrl}
        autoPlay={!mixedEnabled}
        crossOrigin='anonymous'
        muted={mixedEnabled}
        preload='metadata'
        onClick={interactions.handleVideoClick}
        onDoubleClick={interactions.handleVideoDoubleClick}
        onPlay={() => {
          setPlaying(true);
          if (!mixedEnabled) return;
          if (mixed.primingPlayRef.current) {
            mixed.stop();
            mixed.ensureBuffered();
            return;
          }
          const index = mixed.indexForTime(videoRef.current?.currentTime ?? 0);
          if (!mixed.hasChunk(index)) {
            videoRef.current?.pause();
            setPlaying(false);
            void mixed.playWhenReady();
            return;
          }
          void mixed.restart();
        }}
        onPause={() => {
          setPlaying(false);
          if (mixedEnabled) mixed.stop();
        }}
        onLoadedMetadata={(event) => {
          if (Number.isFinite(event.currentTarget.duration)) setDuration(event.currentTarget.duration);
          if (mixedEnabled) {
            mixed.ensureBuffered();
            void mixed.playWhenReady();
          }
        }}
        onTimeUpdate={(event) => {
          setCurrentTime(event.currentTarget.currentTime);
          if (mixedEnabled) mixed.ensureBuffered();
        }}
        onSeeking={() => {
          if (!mixedEnabled) return;
          mixed.cancelPlayRequest();
          mixed.resetTimeline();
        }}
        onSeeked={(event) => {
          setCurrentTime(event.currentTarget.currentTime);
          if (interactions.keyboardSeekingRef.current) return;
          if (playbackRequestedRef.current) void mixed.playWhenReady();
          else if (mixedEnabled) mixed.ensureBuffered();
        }}
        onRateChange={(event) => {
          if (mixedEnabled && !event.currentTarget.paused) void mixed.restart();
        }}
        onEnded={() => {
          playbackRequestedRef.current = false;
          setPlaying(false);
          if (mixedEnabled) mixed.stop();
          interactions.endFastHold();
        }}
      />
      {interactions.holdingFast && <div className='speed-pill'>2x</div>}
      {interactions.seekFeedback && <SeekFeedbackOverlay feedback={interactions.seekFeedback} />}
      <PlayerControls
        currentTime={currentTime}
        duration={duration}
        playing={playing}
        volume={volume}
        onSeek={interactions.seekToPercent}
        onTogglePlayback={interactions.togglePlayback}
        onToggleFullscreen={interactions.toggleFullscreen}
        onVolumeChange={setVolume}
      />
    </div>
  );
}
