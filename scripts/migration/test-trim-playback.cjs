'use strict';
// Trim editor playback cost: one frame loop per view however long the clip
// plays, a playhead frame that copies the still waveform instead of drawing
// it again, and handle drags that never stack seeks on the decoder.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const root = path.resolve(__dirname, '../..');
const player = 'src/renderer/features/player/';

function load(relative, stubs = {}) {
  const file = path.resolve(root, relative);
  const exports = {};
  const code = ts.transpileModule(fs.readFileSync(file, 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020, jsx: ts.JsxEmit.ReactJSX }
  }).outputText;
  new Function('exports', 'require', code)(exports, name => {
    if (name in stubs) return stubs[name];
    const base = path.resolve(path.dirname(file), name);
    const found = ['.ts', '.tsx'].map(extension => base + extension).find(candidate => fs.existsSync(candidate));
    assert.ok(found, 'Unexpected dependency: ' + name);
    return load(found, stubs);
  });
  return exports;
}

// Animation frames run only when the test says, so pending work is countable.
const frames = new Map();
let nextFrame = 0, mostPending = 0;
const requestAnimationFrame = callback => {
  frames.set(++nextFrame, callback);
  mostPending = Math.max(mostPending, frames.size);
  return nextFrame;
};
const cancelAnimationFrame = handle => { frames.delete(handle); };
const runFrame = () => {
  const due = [...frames.values()];
  frames.clear();
  due.forEach(callback => callback(0));
};
const resetFrames = () => { frames.clear(); mostPending = 0; };

class Video extends EventTarget {
  paused = true; seeking = false; seeks = []; time = 0;
  get currentTime() { return this.time; }
  set currentTime(value) { this.seeks.push(value); this.time = value; this.seeking = true; }
  /** The decoder finishes the seek in flight. */
  land() { this.seeking = false; this.dispatchEvent(new Event('seeked')); }
  play() { this.paused = false; this.dispatchEvent(new Event('play')); }
  pause() { this.paused = true; this.dispatchEvent(new Event('pause')); }
  advance(seconds) { this.time += seconds; }
  emit(name) { this.dispatchEvent(new Event(name)); }
}

// Canvas, style and path doubles that count the work a frame does.
const work = { styles: 0, paths: 0, calls: {} };
const context = new Proxy({}, {
  get: (target, key) => key in target ? target[key] : (...args) => {
    work.calls[key] = (work.calls[key] || 0) + 1;
    return key === 'measureText' ? { width: 20 } : undefined;
  },
  set: (target, key, value) => { target[key] = value; return true; }
});
const canvas = () => ({ width: 0, height: 0, style: {}, getContext: () => context, addEventListener() {}, removeEventListener() {} });
let resized = [];
Object.assign(global, { requestAnimationFrame, cancelAnimationFrame });
global.window = { requestAnimationFrame, cancelAnimationFrame, devicePixelRatio: 1 };
global.document = { documentElement: {}, createElement: canvas };
global.getComputedStyle = () => { work.styles++; return { getPropertyValue: () => '' }; };
global.Path2D = class { constructor() { work.paths++; } moveTo() {} lineTo() {} closePath() {} };
global.ResizeObserver = class { constructor(callback) { resized.push(callback); } observe() {} disconnect() {} };
global.MutationObserver = class { observe() {} disconnect() {} };

/** A hooks double: refs and state hold, effects run when `mount` says. */
function hooks() {
  const effects = [];
  return {
    effects,
    react: {
      useRef: value => ({ current: value }),
      useState: value => [typeof value === 'function' ? value() : value, () => {}],
      useEffect: effect => effects.push(effect)
    }
  };
}
const jsxRuntime = { jsx: (type, props) => ({ type, props }), jsxs: (type, props) => ({ type, props }), Fragment: 'fragment' };
const icons = new Proxy({}, { get: () => () => null });

function find(node, test) {
  if (!node || typeof node !== 'object') return undefined;
  if (Array.isArray(node)) {
    for (const child of node) { const hit = find(child, test); if (hit) return hit; }
    return undefined;
  }
  if (node.props && test(node)) return node;
  return node.props ? find(node.props.children, test) : undefined;
}
const text = node => Array.isArray(node) ? node.map(text).join('') : typeof node === 'string' ? node : node?.props ? text(node.props.children) : '';

const sampleRate = 48000, samplesPerPixel = 256, clipSeconds = 60;
const pairs = Math.ceil(clipSeconds * sampleRate / samplesPerPixel);
const waveform = {
  version: 2, channels: 1, sample_rate: sampleRate, samples_per_pixel: samplesPerPixel, bits: 8, length: pairs,
  data: Array.from({ length: pairs * 2 }, (_, index) => (index % 2 ? 1 : -1) * ((index * 37) % 120))
};

function testFollowPlayback() {
  const { followPlayback } = load(player + 'followPlayback.ts');
  resetFrames();
  const video = new Video();
  let calls = 0;
  const stop = followPlayback(video, () => calls++);
  assert.equal(frames.size, 0, 'A paused video schedules nothing');

  video.emit('seeked'); video.emit('timeupdate'); video.emit('seeked');
  assert.equal(frames.size, 1, 'A burst of events while paused asks for one frame');
  runFrame();
  assert.deepEqual([calls, frames.size], [1, 0], 'A paused video draws once and stops');

  video.play();
  for (let frame = 0; frame < 1800; frame++) {
    // About four timeupdates a second, and a seek now and then, as in a real clip.
    if (frame % 15 === 0) video.emit('timeupdate');
    if (frame % 240 === 0) { video.currentTime = video.time; video.land(); }
    runFrame();
  }
  assert.equal(mostPending, 1, 'Events during playback must not start more frame loops');
  assert.equal(calls, 1801, 'Exactly one call per frame over 30 s of playback');

  video.pause();
  runFrame();
  assert.equal(frames.size, 0, 'Pausing ends the loop');
  stop();
  video.play(); video.emit('timeupdate');
  assert.equal(frames.size, 0, 'Stopping removes every listener');
  console.log('Follow playback: one pending frame across 30 s of events, single frame when paused, clean stop.');
}

function testSeekCoalesced() {
  const { seekCoalesced } = load(player + 'seekCoalesced.ts');
  const video = new Video();
  seekCoalesced(video, 1);
  assert.deepEqual(video.seeks, [1], 'An idle video seeks at once');

  // A handle drag: sixty moves while the first seek is still decoding.
  for (let move = 1; move <= 60; move++) seekCoalesced(video, 1 + move / 10);
  assert.deepEqual(video.seeks, [1], 'Nothing new reaches the decoder mid-seek');
  video.land();
  assert.deepEqual(video.seeks, [1, 7], 'Only the latest target follows when the seek lands');
  video.land();
  assert.deepEqual(video.seeks, [1, 7], 'Nothing is left queued');
  assert.equal(video.currentTime, 7);

  seekCoalesced(video, 7);
  assert.deepEqual(video.seeks, [1, 7], 'Seeking to where the video is does nothing');

  // A seek replaced before it lands (the decoder never reported it) must not
  // leave later targets stuck behind it.
  seekCoalesced(video, 3);
  seekCoalesced(video, 4);
  video.seeking = false;
  seekCoalesced(video, 5);
  assert.deepEqual(video.seeks.slice(-2), [3, 5], 'An idle video always seeks straight away');
  video.land();
  assert.deepEqual(video.seeks.slice(-2), [3, 5], 'The superseded target is dropped');
  console.log('Seek coalescing: one seek in flight, last target wins, no stuck targets.');
}

function testDetailViewFrames() {
  const { react, effects } = hooks();
  const { WaveformZoom } = load(player + 'WaveformZoom.tsx', { react, 'react/jsx-runtime': jsxRuntime });
  resetFrames();
  resized = [];
  Object.assign(work, { styles: 0, paths: 0, calls: {} });
  const video = new Video();
  const views = [];
  const element = WaveformZoom({
    waveform, duration: clipSeconds, seconds: 8, viewStart: 0, onViewStart: start => views.push(start),
    range: { start: 1, end: 50 }, onRange() {}, video: { current: video }, onSeek() {}
  });
  element.props.ref.current = canvas();
  const cleanups = effects.map(effect => effect());
  resized.forEach(callback => callback([{ contentRect: { width: 600, height: 96 } }]));
  assert.deepEqual([work.styles, work.paths], [1, 1], 'The first draw reads the theme and builds the waveform once');
  const measured = work.calls.measureText;

  video.play();
  for (let frame = 0; frame < 600; frame++) {
    video.advance(1 / 60);
    if (frame % 15 === 0) video.emit('timeupdate');
    runFrame();
  }
  assert.equal(mostPending, 1, 'The detail view runs one frame loop');
  assert.deepEqual([work.styles, work.paths], [1, 1], 'Playhead frames must not restyle or rebuild the waveform');
  assert.equal(work.calls.drawImage, 1 + 600, 'The first draw and each frame copy the still layer once');
  assert.equal(work.calls.measureText, measured, 'The axis is not measured again every frame');
  assert.ok(views.length > 0, 'The view still turns the page as the playhead passes');

  window.devicePixelRatio = 2;
  video.emit('timeupdate'); runFrame();
  assert.equal(work.styles, 2, 'A new screen density redraws the still layer');
  window.devicePixelRatio = 1;

  video.pause(); runFrame();
  assert.equal(frames.size, 0);
  cleanups.forEach(cleanup => cleanup?.());
  video.play();
  assert.equal(frames.size, 0, 'Closing the editor stops its frame loop');
  console.log('Detail view: 600 playing frames, theme read and waveform built once, one loop, page turns kept.');
}

function testTrimEditor() {
  const { react, effects } = hooks();
  const overview = () => null;
  const { TrimEditor, useTrimLoop } = load(player + 'TrimEditor.tsx', {
    react, 'react/jsx-runtime': jsxRuntime, 'lucide-react': icons,
    './WaveformOverview': { WaveformOverview: overview },
    './WaveformZoom': { WaveformZoom: overview, ZOOM_STEPS: [2, 4, 8, 16, 32] }
  });
  resetFrames();
  const video = new Video();
  video.time = 12.3;
  const changes = [];
  const tree = TrimEditor({
    video: { current: video }, duration: clipSeconds, range: { start: 0, end: clipSeconds }, waveform,
    busy: false, error: '', onChange: range => changes.push(range), onSeek() {}, onCancel() {}, onApply() {}
  });
  const strip = find(tree, node => node.props.className === 'trim-track');
  const placed = [];
  strip.props.ref.current = { style: { setProperty: (name, value) => placed.push([name, value]) } };
  assert.equal(strip.props.style['--trim-playhead'], undefined, 'React no longer re-renders the strip playhead');
  const cleanups = effects.map(effect => effect());
  assert.deepEqual(placed.at(-1), ['--trim-playhead', '20.5%'], 'The strip playhead is placed on open');

  video.play();
  for (let frame = 0; frame < 30; frame++) { video.advance(0.1); runFrame(); }
  assert.equal(placed.length, 31, 'The strip playhead moves every frame while playing');
  assert.equal(mostPending, 1);
  video.pause(); runFrame();

  // The buttons cut at the frame on screen, not at the last timeupdate.
  video.time = 31.25;
  find(tree, node => node.type === 'button' && text(node) === 'Start here').props.onClick();
  video.time = 40.5;
  find(tree, node => node.type === 'button' && text(node) === 'End here').props.onClick();
  assert.deepEqual(changes.map(range => [range.start, range.end]), [[31.25, clipSeconds], [31.25, 40.5]]);
  cleanups.forEach(cleanup => cleanup?.());

  const loop = hooks();
  const { useTrimLoop: loopHook } = load(player + 'TrimEditor.tsx', {
    react: loop.react, 'react/jsx-runtime': jsxRuntime, 'lucide-react': icons,
    './WaveformOverview': { WaveformOverview: overview },
    './WaveformZoom': { WaveformZoom: overview, ZOOM_STEPS: [2, 4, 8, 16, 32] }
  });
  assert.equal(typeof useTrimLoop, 'function');
  resetFrames();
  const looped = new Video();
  looped.time = 3;
  loopHook({ current: looped }, { start: 2, end: 5 });
  const stop = loop.effects[0]();
  assert.equal(frames.size, 0, 'The trim loop sleeps while paused');
  looped.play();
  runFrame();
  assert.deepEqual(looped.seeks, [], 'Inside the kept part nothing moves');
  looped.advance(2.05); runFrame();
  assert.deepEqual(looped.seeks, [2], 'Reaching the end loops back to the start');
  looped.land(); runFrame();
  assert.equal(mostPending, 1);
  stop();
  assert.equal(frames.size, 0);
  console.log('Trim strip: per-frame playhead, live-time Start/End here, loop only while playing.');
}

testFollowPlayback();
testSeekCoalesced();
testDetailViewFrames();
testTrimEditor();
