(async () => {
  if (window.__cliptureSmokeStarted) return;
  window.__cliptureSmokeStarted = true;
  const { invoke, convertFileSrc } = window.__TAURI_INTERNALS__;
  const checks = [];
  const assert = (condition, name) => { if (!condition) throw new Error(name); checks.push(name); };
  const timeout = (promise, ms = 15000) => Promise.race([promise, new Promise((_, reject) => setTimeout(() => reject(new Error('operation timed out')), ms))]);
  try {
    for (let i = 0; i < 100 && !document.querySelector('#root button'); i++) await new Promise(resolve => setTimeout(resolve, 100));
    assert(!!document.querySelector('#root button'), 'React UI mounted');
    const host = await invoke('host_info');
    assert(host.host === 'tauri' && host.testMode, 'real Tauri IPC with isolated profile');
    const burst = await timeout(Promise.allSettled(Array.from({ length: 24 }, () => invoke('get_settings'))), 5000);
    assert(burst.some(result => result.status === 'fulfilled') && burst.every(result =>
      result.status === 'fulfilled' || /busy|limit reached/.test(String(result.reason))),
      'concurrent IPC burst settles without lost replies');
    const settings = await invoke('get_settings');
    const saved = await invoke('save_settings', { settings: { ...settings, bitrateMbps: 17 } });
    assert(saved.bitrateMbps === 17, 'settings round trip');
    const clips = await invoke('list_clips');
    assert(clips.length === 1, 'fixture library discovered');
    const clip = clips[0];
    const session = await invoke('clip_playback_url', { filePath: clip.filePath, audioTracks: clip.audioTracks });
    const response = await timeout(fetch(session.url, { headers: { Range: 'bytes=0-31' } }));
    assert(response.status === 206 && (await response.arrayBuffer()).byteLength === 32, 'media byte range served through WebView');
    const video = document.createElement('video');
    video.muted = true;
    video.src = session.url;
    document.body.append(video);
    await timeout(new Promise((resolve, reject) => { video.onloadedmetadata = resolve; video.onerror = () => reject(new Error(`video decode error ${video.error?.code}`)); }));
    assert(video.videoWidth === 320 && video.duration > 1, 'WebView decoded fixture video');
    video.remove();
    video.removeAttribute('src');
    video.load();
    const audio = await timeout(fetch(`${session.audioChunkUrl}?start=0&duration=1`));
    assert(audio.ok && (await audio.arrayBuffer()).byteLength > 1000, 'native FFmpeg mixed both audio tracks');
    const thumbnail = await invoke('clip_thumbnail_url', { filePath: clip.filePath });
    assert(thumbnail.startsWith('data:image/'), 'thumbnail generated');
    const sounds = await invoke('list_clip_sounds');
    assert(sounds.length >= 2, 'bundled sounds available');
    const preview = new Audio(convertFileSrc(sounds[0].url, 'asset'));
    await timeout(new Promise((resolve, reject) => { preview.onloadedmetadata = resolve; preview.onerror = () => reject(new Error(`sound preview error ${preview.error?.code}`)); preview.load(); }));
    assert(preview.duration > 0, 'scoped sound asset decoded');
    preview.removeAttribute('src'); preview.load();
    let denied = false;
    try { await invoke('clip_url', { filePath: clip.filePath + '.unauthorized' }); } catch { denied = true; }
    assert(denied, 'arbitrary media path rejected');
    await invoke('release_playback_cache');
    assert((await fetch(session.url)).status === 404, 'released media session rejected');
    assert(!(await invoke('save_clip', { durationSeconds: 5 })).ok, 'test mode cannot start capture');
    assert((await invoke('check_for_updates')).status === 'idle', 'test mode updater stays offline');
    await invoke('plugin:event|emit', { event: 'clipture-smoke-result', payload: { ok: true, checks } });
  } catch (error) {
    await invoke('plugin:event|emit', { event: 'clipture-smoke-result', payload: { ok: false, checks, error: String(error) } });
  }
})();
