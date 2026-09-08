import { useEffect, useRef } from 'react';
import type { MutableRefObject, RefObject } from 'react';

interface MixedAudioChunk {
  start: number;
  buffer: AudioBuffer;
  lastUsedAt: number;
}

interface MixedAudioChunkRequest {
  promise: Promise<void>;
  controller: AbortController;
}

interface MixedAudioOptions {
  videoRef: RefObject<HTMLVideoElement>;
  enabled: boolean;
  chunkUrl: string;
  chunkSeconds: number;
  duration: number;
  sourceUrl: string;
  volume: number;
  playbackRequestedRef: MutableRefObject<boolean>;
  onPlayingChange: (playing: boolean) => void;
}

const scheduleLookaheadSeconds = 0.5;
const schedulerIntervalMs = 100;

export function useMixedAudioPlayback({
  videoRef,
  enabled,
  chunkUrl,
  chunkSeconds,
  duration,
  sourceUrl,
  volume,
  playbackRequestedRef,
  onPlayingChange
}: MixedAudioOptions) {
  const audioContextRef = useRef<AudioContext | null>(null);
  const gainNodeRef = useRef<GainNode | null>(null);
  const connectedVideoRef = useRef<HTMLVideoElement | null>(null);
  const chunkCacheRef = useRef(new Map<number, MixedAudioChunk>());
  const chunkRequestsRef = useRef(new Map<number, MixedAudioChunkRequest>());
  const scheduledChunksRef = useRef(new Map<number, AudioBufferSourceNode[]>());
  const timerRef = useRef<number | null>(null);
  const generationRef = useRef(0);
  const playRequestRef = useRef(0);
  const primingPlayRef = useRef(false);

  function ensureAudioContext() {
    if (!audioContextRef.current || audioContextRef.current.state === 'closed') {
      const AudioContextCtor = window.AudioContext || (window as typeof window & { webkitAudioContext: typeof AudioContext }).webkitAudioContext;
      audioContextRef.current = new AudioContextCtor();
      gainNodeRef.current = null;
      connectedVideoRef.current = null;
    }
    if (!gainNodeRef.current || gainNodeRef.current.context !== audioContextRef.current) {
      gainNodeRef.current = audioContextRef.current.createGain();
      gainNodeRef.current.connect(audioContextRef.current.destination);
    }
    return audioContextRef.current;
  }

  function stopNodes(nodes: AudioBufferSourceNode[]) {
    for (const node of nodes) {
      try { node.stop(); } catch { /* already stopped */ }
      try { node.disconnect(); } catch { /* already disconnected */ }
    }
  }

  function stopChunk(index: number) {
    const nodes = scheduledChunksRef.current.get(index);
    if (!nodes) return;
    stopNodes(nodes);
    scheduledChunksRef.current.delete(index);
  }

  function stop() {
    for (const nodes of scheduledChunksRef.current.values()) stopNodes(nodes);
    scheduledChunksRef.current.clear();
  }

  function resetTimeline() {
    generationRef.current += 1;
    stop();
    for (const request of chunkRequestsRef.current.values()) request.controller.abort();
    chunkRequestsRef.current.clear();
  }

  function clear() {
    resetTimeline();
    chunkCacheRef.current.clear();
  }

  function indexForTime(time: number) {
    return Math.max(0, Math.floor(Math.max(0, time) / Math.max(1, chunkSeconds || 8)));
  }

  function urlForChunk(index: number) {
    const size = Math.max(1, chunkSeconds || 8);
    const start = Math.max(0, index * size);
    const remaining = duration > 0 ? Math.max(0.25, duration - start) : size;
    const separator = chunkUrl.includes('?') ? '&' : '?';
    return chunkUrl + separator + 'start=' + encodeURIComponent(start.toFixed(3)) + '&duration=' + encodeURIComponent(Math.min(size, remaining).toFixed(3));
  }

  function loadChunk(index: number, generation: number) {
    if (!enabled || !chunkUrl || chunkCacheRef.current.has(index)) return Promise.resolve();
    const pending = chunkRequestsRef.current.get(index);
    if (pending) return pending.promise;
    const size = Math.max(1, chunkSeconds || 8);
    const start = index * size;
    if (duration > 0 && start > duration + 0.25) return Promise.resolve();

    const controller = new AbortController();
    let request: Promise<void>;
    request = fetch(urlForChunk(index), { signal: controller.signal }).then(async (response) => {
      if (!response.ok) throw new Error('audio chunk ' + response.status);
      const encoded = await response.arrayBuffer();
      if (generation !== generationRef.current || encoded.byteLength === 0) return;
      const buffer = await ensureAudioContext().decodeAudioData(encoded.slice(0));
      if (generation === generationRef.current) chunkCacheRef.current.set(index, { start, buffer, lastUsedAt: performance.now() });
    }).catch((error) => {
      if (!(error instanceof DOMException && error.name === 'AbortError')) console.warn('Mixed audio chunk failed:', error);
    }).finally(() => {
      if (chunkRequestsRef.current.get(index)?.promise === request) chunkRequestsRef.current.delete(index);
    });
    chunkRequestsRef.current.set(index, { promise: request, controller });
    return request;
  }

  function scheduleChunk(index: number, generation: number) {
    if (generation !== generationRef.current || scheduledChunksRef.current.has(index)) return;
    const video = videoRef.current;
    const chunk = chunkCacheRef.current.get(index);
    if (!video || !chunk || video.paused || video.ended) return;
    const context = ensureAudioContext();
    const gain = gainNodeRef.current;
    if (!gain || context.state !== 'running') return;

    const rate = Math.max(0.1, Math.abs(video.playbackRate || 1));
    const size = Math.max(1, chunkSeconds || 8);
    const nominalEnd = duration > 0 ? Math.min(duration, chunk.start + size) : chunk.start + size;
    const end = Math.min(chunk.start + chunk.buffer.duration, nominalEnd);
    if (video.currentTime >= end - 0.05) return;
    const offset = Math.max(0, Math.min(chunk.buffer.duration - 0.05, video.currentTime - chunk.start));
    const delay = Math.max(0, (chunk.start - video.currentTime) / rate);
    if (delay > scheduleLookaheadSeconds) return;

    const source = context.createBufferSource();
    source.buffer = chunk.buffer;
    source.playbackRate.value = rate;
    source.connect(gain);
    const nodes = [source];
    source.onended = () => { try { source.disconnect(); } catch { /* generation cleanup won */ } };
    scheduledChunksRef.current.set(index, nodes);
    chunk.lastUsedAt = performance.now();
    try {
      source.start(context.currentTime + delay, offset, Math.max(0.01, end - Math.max(video.currentTime, chunk.start)));
    } catch (error) {
      if (scheduledChunksRef.current.get(index) === nodes) scheduledChunksRef.current.delete(index);
      try { source.disconnect(); } catch { /* already disconnected */ }
      console.warn('Mixed audio schedule failed:', error);
    }
  }

  function cleanup(time: number) {
    const size = Math.max(1, chunkSeconds || 8);
    for (const [index, chunk] of chunkCacheRef.current) {
      if (chunk.start + chunk.buffer.duration < time - 10 || chunk.start > time + 24) chunkCacheRef.current.delete(index);
    }
    for (const [index] of scheduledChunksRef.current) {
      if ((index + 1) * size < time - 1) stopChunk(index);
    }
  }

  function ensureBuffered() {
    const video = videoRef.current;
    if (!enabled || !chunkUrl || !video || video.seeking) return;
    const size = Math.max(1, chunkSeconds || 8);
    const time = Math.max(0, video.currentTime);
    const generation = generationRef.current;
    const first = Math.max(0, Math.floor(Math.max(0, time - 0.25) / size));
    const last = Math.max(first, Math.floor((time + 16) / size));
    let prefetch = Promise.resolve();
    for (let index = first; index <= last; index += 1) {
      scheduleChunk(index, generation);
      prefetch = prefetch.then(() => loadChunk(index, generation)).then(() => scheduleChunk(index, generation));
    }
    void prefetch;
    cleanup(time);
  }

  async function restart() {
    if (!enabled || !chunkUrl) return;
    stop();
    try { await ensureAudioContext().resume(); } catch (error) { console.warn('Mixed audio resume failed:', error); }
    ensureBuffered();
  }

  async function playWhenReady() {
    const video = videoRef.current;
    if (!video || !playbackRequestedRef.current) return;
    if (!enabled || !chunkUrl) {
      try { await video.play(); } catch (error) { console.warn('Video play failed:', error); }
      return;
    }
    const requestId = ++playRequestRef.current;
    const generation = generationRef.current;
    const index = indexForTime(video.currentTime);
    if (!chunkCacheRef.current.has(index)) {
      primingPlayRef.current = true;
      if (!video.paused) video.pause();
      onPlayingChange(false);
      await loadChunk(index, generation);
      if (requestId !== playRequestRef.current || generation !== generationRef.current || !playbackRequestedRef.current) {
        if (requestId === playRequestRef.current) primingPlayRef.current = false;
        return;
      }
    }
    try { await ensureAudioContext().resume(); } catch (error) { console.warn('Mixed audio resume failed:', error); }
    if (requestId !== playRequestRef.current || generation !== generationRef.current || !playbackRequestedRef.current) return;
    primingPlayRef.current = true;
    try { await video.play(); ensureBuffered(); } catch (error) { console.warn('Video play failed:', error); }
    finally { if (requestId === playRequestRef.current) primingPlayRef.current = false; }
  }

  function cancelPlayRequest() {
    playRequestRef.current += 1;
    primingPlayRef.current = false;
  }

  useEffect(() => {
    const video = videoRef.current;
    if (!video) return;
    if (enabled) {
      video.muted = true;
      video.volume = 0;
      if (gainNodeRef.current) gainNodeRef.current.gain.value = volume;
    } else {
      if (video !== connectedVideoRef.current) {
        try {
          const source = ensureAudioContext().createMediaElementSource(video);
          source.connect(gainNodeRef.current!);
          connectedVideoRef.current = video;
        } catch (error) { console.warn('AudioContext setup failed:', error); }
      }
      video.muted = false;
      video.volume = Math.min(1, volume);
      if (gainNodeRef.current) gainNodeRef.current.gain.value = volume > 1 ? volume : 1;
    }
  }, [volume, sourceUrl, enabled]);

  useEffect(() => {
    if (gainNodeRef.current) gainNodeRef.current.gain.value = enabled ? volume : Math.max(1, volume);
  }, [enabled, volume]);

  useEffect(() => {
    if (!enabled || !chunkUrl || !sourceUrl) return;
    const tick = () => ensureBuffered();
    tick();
    timerRef.current = window.setInterval(tick, schedulerIntervalMs);
    return () => {
      if (timerRef.current) window.clearInterval(timerRef.current);
      timerRef.current = null;
      clear();
    };
  }, [enabled, chunkUrl, chunkSeconds, sourceUrl]);

  useEffect(() => () => {
    clear();
    if (audioContextRef.current?.state !== 'closed') void audioContextRef.current?.close();
  }, []);

  return { cancelPlayRequest, clear, ensureBuffered, hasChunk: (index: number) => chunkCacheRef.current.has(index), indexForTime, playWhenReady, primingPlayRef, resetTimeline, restart, stop };
}

export type MixedAudioPlayback = ReturnType<typeof useMixedAudioPlayback>;
