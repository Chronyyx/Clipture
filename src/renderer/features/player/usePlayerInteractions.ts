import { useCallback, useEffect, useRef, useState } from 'react';
import type { Dispatch, MutableRefObject, RefObject, SetStateAction } from 'react';

interface MixedAudioControls {
  ensureBuffered(): void;
  playWhenReady(): Promise<void>;
  resetTimeline(): void;
  stop(): void;
  cancelPlayRequest(): void;
}

interface PlayerInteractionOptions {
  videoRef: RefObject<HTMLVideoElement>;
  duration: number;
  mixedEnabled: boolean;
  playbackRequestedRef: MutableRefObject<boolean>;
  mixed: MixedAudioControls;
  setCurrentTime: Dispatch<SetStateAction<number>>;
  setPlaying: Dispatch<SetStateAction<boolean>>;
}

export interface SeekFeedback {
  direction: -1 | 1;
  seconds: number;
  sequence: number;
}

export function usePlayerInteractions({
  videoRef,
  duration,
  mixedEnabled,
  playbackRequestedRef,
  mixed,
  setCurrentTime,
  setPlaying
}: PlayerInteractionOptions) {
  const fastHoldActivatedRef = useRef(false);
  const holdTimeoutRef = useRef<number | null>(null);
  const clickTimeoutRef = useRef<number | null>(null);
  const controlsTimeoutRef = useRef<number | null>(null);
  const keyboardDelayRef = useRef<number | null>(null);
  const keyboardIntervalRef = useRef<number | null>(null);
  const keyboardDirectionRef = useRef<-1 | 0 | 1>(0);
  const keyboardStartedAtRef = useRef(0);
  const keyboardSeekingRef = useRef(false);
  const seekTotalRef = useRef(0);
  const seekSequenceRef = useRef(0);
  const seekTimeoutRef = useRef<number | null>(null);
  const [holdingFast, setHoldingFast] = useState(false);
  const [controlsVisible, setControlsVisible] = useState(true);
  const [seekFeedback, setSeekFeedback] = useState<SeekFeedback | null>(null);

  function togglePlayback() {
    const video = videoRef.current;
    if (!video) return;
    if (!video.paused) {
      playbackRequestedRef.current = false;
      mixed.cancelPlayRequest();
      video.pause();
      if (mixedEnabled) mixed.stop();
      return;
    }
    playbackRequestedRef.current = true;
    void mixed.playWhenReady();
  }

  function toggleFullscreen() {
    const shell = videoRef.current?.parentElement;
    if (document.fullscreenElement) void document.exitFullscreen().catch(console.error);
    else void shell?.requestFullscreen().catch(console.error);
  }

  function handleVideoClick() {
    if (fastHoldActivatedRef.current) {
      fastHoldActivatedRef.current = false;
      return;
    }
    if (clickTimeoutRef.current) window.clearTimeout(clickTimeoutRef.current);
    clickTimeoutRef.current = window.setTimeout(() => {
      clickTimeoutRef.current = null;
      togglePlayback();
    }, 220);
  }

  function handleVideoDoubleClick() {
    fastHoldActivatedRef.current = false;
    if (clickTimeoutRef.current) window.clearTimeout(clickTimeoutRef.current);
    clickTimeoutRef.current = null;
    toggleFullscreen();
  }

  function showControls() {
    setControlsVisible(true);
    if (controlsTimeoutRef.current) window.clearTimeout(controlsTimeoutRef.current);
    controlsTimeoutRef.current = window.setTimeout(() => setControlsVisible(false), 2000);
  }

  function hideControls() {
    setControlsVisible(false);
    if (controlsTimeoutRef.current) window.clearTimeout(controlsTimeoutRef.current);
    controlsTimeoutRef.current = null;
  }

  function beginFastHold() {
    fastHoldActivatedRef.current = false;
    if (holdTimeoutRef.current) window.clearTimeout(holdTimeoutRef.current);
    holdTimeoutRef.current = window.setTimeout(() => {
      const video = videoRef.current;
      if (!video) return;
      fastHoldActivatedRef.current = true;
      video.playbackRate = 2;
      setHoldingFast(true);
    }, 500);
  }

  function endFastHold() {
    if (holdTimeoutRef.current) window.clearTimeout(holdTimeoutRef.current);
    holdTimeoutRef.current = null;
    if (videoRef.current) videoRef.current.playbackRate = 1;
    setHoldingFast(false);
  }

  function seekToPercent(percent: number) {
    const video = videoRef.current;
    if (!video || !Number.isFinite(video.duration)) return;
    if (mixedEnabled) mixed.resetTimeline();
    video.currentTime = (Math.min(100, Math.max(0, percent)) / 100) * video.duration;
  }

  const showSeekFeedback = useCallback((actualSeconds: number) => {
    if (Math.abs(actualSeconds) < 0.01) return;
    seekTotalRef.current += actualSeconds;
    const total = seekTotalRef.current;
    setSeekFeedback({
      direction: total < 0 ? -1 : 1,
      seconds: Math.max(1, Math.round(Math.abs(total))),
      sequence: seekSequenceRef.current
    });
    if (seekTimeoutRef.current) window.clearTimeout(seekTimeoutRef.current);
    seekTimeoutRef.current = window.setTimeout(() => {
      seekTimeoutRef.current = null;
      setSeekFeedback(null);
      seekTotalRef.current = 0;
    }, 850);
  }, []);

  const seekBySeconds = useCallback((seconds: number) => {
    const video = videoRef.current;
    if (!video) return;
    const totalDuration = Number.isFinite(video.duration) && video.duration > 0 ? video.duration : duration;
    if (!Number.isFinite(totalDuration) || totalDuration <= 0) return;
    const previous = Math.max(0, video.currentTime);
    const next = Math.min(totalDuration, Math.max(0, previous + seconds));
    if (Math.abs(next - previous) < 0.01) return;
    video.currentTime = next;
    setCurrentTime(next);
    showSeekFeedback(next - previous);
  }, [duration, setCurrentTime, showSeekFeedback, videoRef]);

  useEffect(() => {
    function clearKeyboardTimers() {
      if (keyboardDelayRef.current) window.clearTimeout(keyboardDelayRef.current);
      if (keyboardIntervalRef.current) window.clearInterval(keyboardIntervalRef.current);
      keyboardDelayRef.current = null;
      keyboardIntervalRef.current = null;
    }
    function finishKeyboardSeek() {
      if (keyboardDirectionRef.current === 0) return;
      clearKeyboardTimers();
      keyboardDirectionRef.current = 0;
      keyboardSeekingRef.current = false;
      const video = videoRef.current;
      if (!video || video.seeking) return;
      if (playbackRequestedRef.current) void mixed.playWhenReady();
      else if (mixedEnabled) mixed.ensureBuffered();
    }
    function onKeyDown(event: globalThis.KeyboardEvent) {
      const target = event.target as HTMLElement | null;
      const interactive = target?.closest('input, textarea, select, button, a, [contenteditable=true]');
      if ((event.code === 'Space' || event.key === ' ') && !interactive) {
        if (event.ctrlKey || event.altKey || event.metaKey) return;
        event.preventDefault();
        if (!event.repeat) togglePlayback();
        return;
      }
      if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
      if (target?.closest('input, textarea, select, [contenteditable=true]')) return;
      if (event.ctrlKey || event.altKey || event.metaKey || event.repeat) return;
      event.preventDefault();
      finishKeyboardSeek();
      const direction: -1 | 1 = event.key === 'ArrowLeft' ? -1 : 1;
      keyboardDirectionRef.current = direction;
      keyboardStartedAtRef.current = performance.now();
      keyboardSeekingRef.current = true;
      seekTotalRef.current = 0;
      seekSequenceRef.current += 1;
      setSeekFeedback(null);
      seekBySeconds(direction * 5);
      keyboardDelayRef.current = window.setTimeout(() => {
        const seekAgain = () => {
          const heldMs = performance.now() - keyboardStartedAtRef.current;
          seekBySeconds(direction * (heldMs >= 2000 ? 20 : heldMs >= 900 ? 10 : 5));
        };
        seekAgain();
        keyboardIntervalRef.current = window.setInterval(seekAgain, 120);
      }, 300);
    }
    function onKeyUp(event: globalThis.KeyboardEvent) {
      const direction = event.key === 'ArrowLeft' ? -1 : event.key === 'ArrowRight' ? 1 : 0;
      if (direction !== 0 && direction === keyboardDirectionRef.current) finishKeyboardSeek();
    }
    const onVisibility = () => { if (document.hidden) finishKeyboardSeek(); };
    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('keyup', onKeyUp);
    window.addEventListener('blur', finishKeyboardSeek);
    document.addEventListener('visibilitychange', onVisibility);
    return () => {
      clearKeyboardTimers();
      keyboardDirectionRef.current = 0;
      keyboardSeekingRef.current = false;
      if (seekTimeoutRef.current) window.clearTimeout(seekTimeoutRef.current);
      window.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('keyup', onKeyUp);
      window.removeEventListener('blur', finishKeyboardSeek);
      document.removeEventListener('visibilitychange', onVisibility);
    };
  }, [mixedEnabled, seekBySeconds]);

  useEffect(() => {
    const stopFastHold = () => endFastHold();
    window.addEventListener('pointerup', stopFastHold);
    window.addEventListener('blur', stopFastHold);
    document.addEventListener('visibilitychange', stopFastHold);
    return () => {
      window.removeEventListener('pointerup', stopFastHold);
      window.removeEventListener('blur', stopFastHold);
      document.removeEventListener('visibilitychange', stopFastHold);
      if (controlsTimeoutRef.current) window.clearTimeout(controlsTimeoutRef.current);
      if (clickTimeoutRef.current) window.clearTimeout(clickTimeoutRef.current);
    };
  }, []);

  return {
    beginFastHold,
    controlsVisible,
    endFastHold,
    handleVideoClick,
    handleVideoDoubleClick,
    hideControls,
    holdingFast,
    keyboardSeekingRef,
    seekFeedback,
    seekToPercent,
    showControls,
    toggleFullscreen,
    togglePlayback
  };
}

export type PlayerInteractions = ReturnType<typeof usePlayerInteractions>;
