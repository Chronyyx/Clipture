// Exercise real player cleanup/scheduling with deterministic media doubles.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const root = path.resolve(__dirname, '../..');
function load(relative, stubs = {}) {
  const file = path.resolve(root, relative);
  const exports = {};
  const code = ts.transpileModule(fs.readFileSync(file, 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 }
  }).outputText;
  new Function('exports', 'require', code)(exports, name => {
    if (name in stubs) return stubs[name];
    return load(path.resolve(path.dirname(file), name + '.ts'), stubs);
  });
  return exports;
}
const timers = new Map();
let nextTimer = 0, contexts = 0, closed = 0, disconnected = 0;
class Context {
  state = 'running';
  constructor() { contexts++; }
  createGain() { return { context: this, gain: { value: 1 }, connect() {}, disconnect() { disconnected++; } }; }
  createMediaElementSource() { return { connect() {} }; }
  resume() { this.state = 'running'; return Promise.resolve(); }
  suspend() { this.state = 'suspended'; return Promise.resolve(); }
  close() { this.state = 'closed'; closed++; return Promise.resolve(); }
}
global.document = { hidden: false };
global.window = { AudioContext: Context,
  setInterval: fn => { timers.set(++nextTimer, fn); return nextTimer; },
  clearInterval: id => timers.delete(id) };
class Video extends EventTarget {
  paused = true; ended = false; seeking = false; currentTime = 0; playbackRate = 1;
  srcObject = {}; operations = [];
  play() { this.paused = false; this.dispatchEvent(new Event('play')); return Promise.resolve(); }
  pause() { this.operations.push('pause'); if (!this.paused) { this.paused = true; this.dispatchEvent(new Event('pause')); } }
  removeAttribute(name) { this.operations.push('remove:' + name); }
  load() { this.operations.push('load'); }
}
const drain = async () => { for (let i = 0; i < 30; i++) await Promise.resolve(); };
function mount({ video = new Video(), enabled = false, volume = 1 } = {}) {
  const effects = [];
  const react = { useRef: value => ({ current: value }), useEffect: fn => effects.push(fn) };
  const { useMixedAudioPlayback } = load('src/renderer/features/player/useMixedAudioPlayback.ts', { react });
  const controls = useMixedAudioPlayback({ videoRef: { current: video }, enabled, volume,
    chunkUrl: enabled ? 'https://fixture/audio' : '', sourceUrl: 'fixture:video',
    chunkSeconds: 8, duration: 120, playbackRequestedRef: { current: true }, onPlayingChange() {} });
  const cleanups = effects.map(fn => fn());
  return { controls, close: () => cleanups.forEach(fn => fn?.()) };
}
async function run() {
  const { unloadVideo } = load('src/renderer/features/player/videoLifetime.ts');
  const video = new Video();
  unloadVideo(video);
  assert.deepEqual(video.operations, ['pause', 'remove:src', 'load']);
  assert.equal(video.srcObject, null);

  const normal = mount();
  assert.equal(contexts, 0, 'Normal-volume video needs no Web Audio graph');
  assert.equal(timers.size, 0);
  normal.close();
  const boosted = mount({ volume: 2 });
  assert.equal(contexts, 1, 'Boost retains Web Audio support');
  boosted.close();
  assert.equal(closed, 1);
  assert.equal(disconnected, 1);

  const { watchPlaybackActivity } = load('src/renderer/features/player/playbackActivity.ts');
  const context = new Context();
  let buffered = 0, idled = 0;
  const dispose = watchPlaybackActivity(video, { mixed: true, context: () => context,
    buffer: () => buffered++, idle: () => idled++ });
  assert.equal(timers.size, 0, 'Paused video must not start the 100ms scheduler');
  await video.play();
  assert.equal(timers.size, 1);
  video.pause();
  assert.equal(timers.size, 0);
  assert.equal(context.state, 'suspended');
  assert.equal(idled, 1);
  await video.play();
  assert.equal(context.state, 'running');
  assert.equal(timers.size, 1);
  dispose();
  assert.equal(timers.size, 0);
  await video.play();
  assert.equal(buffered, 2, 'Disposed listeners cannot restart work');

  let requests = 0, aborted = 0;
  global.fetch = (_url, { signal }) => {
    requests++;
    return new Promise((_resolve, reject) => signal.addEventListener('abort', () => {
      aborted++; reject(new DOMException('Canceled', 'AbortError'));
    }));
  };
  const mixedVideo = new Video();
  mixedVideo.paused = false;
  const mixed = mount({ video: mixedVideo, enabled: true });
  await drain();
  assert.equal(requests, 1);
  mixed.controls.primingPlayRef.current = true;
  mixedVideo.pause();
  assert.equal(aborted, 0, 'An internal priming pause must not cancel its needed chunk');
  mixed.close();
  await drain();
  assert.equal(aborted, 1);
  assert.equal(requests, 1, 'Queued prefetches must not fetch after teardown');
  assert.equal(timers.size, 0);

  document.hidden = true;
  const hidden = mount({ video: mixedVideo, enabled: true });
  hidden.controls.ensureBuffered();
  await drain();
  assert.equal(requests, 1, 'Hidden UI must not prefetch audio');
  hidden.close();
  document.hidden = false;
  console.log('Player lifetime passed: decoder reset, lazy boost graph, pause/resume scheduling, priming, canceled prefetch and hidden UI.');
}
run().catch(error => { console.error(error); process.exitCode = 1; });
