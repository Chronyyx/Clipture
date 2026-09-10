// Isolated synthetic media only: no screen capture, live engine, user library or settings.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const { verifyAlignedMp4, verifyInPlaceMp4 } = require('./mp4-layout-fixture.cjs');
const root = path.resolve(__dirname, '../..');
const ffmpeg = require('ffmpeg-static');
const binary = path.join(root, 'build/engine/tests/replay/Release/clipture_replay_tests.exe');
assert.ok(fs.existsSync(binary), 'Build clipture_replay_tests first; see engine/tests/replay/README.md');
const cache = path.join(root, '.cache');
fs.mkdirSync(cache, { recursive: true });
const scratch = fs.mkdtempSync(path.join(cache, 'mp4-ready-'));
const run = (exe, args) => {
  const result = spawnSync(exe, args, { encoding: 'utf8', windowsHide: true, timeout: 60000, maxBuffer: 8 * 1024 * 1024 });
  if (result.error) throw result.error;
  assert.equal(result.status, 0, `${exe} failed: ${result.stderr}`);
  return result;
};
const fixture = path.join(scratch, 'input.h264');
run(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-nostdin', '-f', 'lavfi', '-i',
  'testsrc2=size=160x90:rate=30', '-t', '122', '-an', '-c:v', 'libx264', '-threads', '1',
  '-preset', 'ultrafast', '-g', '30', '-bf', '0', '-x264-params', 'aud=1:repeat-headers=1',
  '-f', 'h264', fixture]);
const frameRows = text => text.split(/\r?\n/).filter(line => line && !line.startsWith('#'));
const frameHash = row => row.split(',').at(-1).trim();
const originalFrames = frameRows(run(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-nostdin',
  '-i', fixture, '-an', '-threads', '1', '-f', 'framemd5', '-']).stdout);
assert.equal(originalFrames.length, 3660);
// The muxed presentation starts 1.5s into the source, between its one-second keyframes.
const expectedHashes = originalFrames.slice(45, 3645).map(frameHash);
const rows = [];
const alignedRows = [];
const inPlaceRows = [];
for (let trial = 0; trial < 3; trial++) {
  const output = path.join(scratch, `trial-${trial}`);
  const result = run(binary, ['--fixture', fixture, output]);
  fs.writeFileSync(path.join(output, 'mux.log'), result.stderr);
  fs.writeFileSync(path.join(output, 'extent.log'), result.stdout);
  if (trial === 0) require('./in-place-audio-fixture.cjs')({ root: path.join(output, 'audio'),
    ffmpeg, run, sourceHashes: originalFrames.map(frameHash) });
  const metrics = JSON.parse(fs.readFileSync(path.join(output, 'metrics.json'), 'utf8'));
  let reference;
  for (const mode of ['legacy', 'prepared', 'mixed']) {
    const folder = path.join(output, mode, 'output');
    const files = fs.readdirSync(folder).filter(name => name.endsWith('.mp4'));
    assert.equal(files.length, 1);
    const decoded = run(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-nostdin', '-i',
      path.join(folder, files[0]), '-map', '0:v:0', '-an', '-threads', '1', '-f', 'framemd5', '-']).stdout;
    const frames = frameRows(decoded);
    assert.equal(frames.length, 3600, `${mode}: 120 seconds at 30fps must decode to 3600 frames`);
    assert.deepEqual(frames.map(frameHash), expectedHashes, `${mode}: wrong visible source interval`);
    if (reference) assert.deepEqual(frames, reference, `${mode}: decoded frames/timestamps differ`);
    reference = frames;
    const value = metrics[mode];
    rows.push({ trial, mode, persistMs: +value.persistenceMs.toFixed(2), muxMs: value.mux.elapsedMs,
      replayBytes: value.replayDiskBytes, outputBytes: value.mux.finalFileBytes,
      readCalls: value.mux.timeline.reduce((sum, bucket) => sum + bucket.replayReadCalls, 0),
      writeBytes: value.mux.timeline.reduce((sum, bucket) => sum + bucket.outputWriteBytes, 0) });
  }
  const extentFrames = frameRows(run(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-nostdin',
    '-i', path.join(output, 'prepared/extent-copy.mp4'), '-an', '-threads', '1', '-f', 'framemd5', '-']).stdout);
  assert.deepEqual(extentFrames, reference, 'extent-copy: decoded frames/timestamps differ');
  assert.deepEqual(extentFrames.map(frameHash), expectedHashes, 'extent-copy: wrong visible source interval');
  const nativeFrames = frameRows(run(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-nostdin',
    '-i', path.join(output, 'prepared/native-file-copy.mp4'), '-an', '-threads', '1', '-f', 'framemd5', '-']).stdout);
  assert.deepEqual(nativeFrames, reference, 'native clone-or-copy output: decoded frames/timestamps differ');
  const preparedFolder = path.join(output, 'prepared/output');
  const preparedFile = path.join(preparedFolder, fs.readdirSync(preparedFolder).find(name => name.endsWith('.mp4')));
  const inPlaceFolder = path.join(output, 'in-place/output');
  const inPlaceFiles = fs.readdirSync(inPlaceFolder).filter(name => name.endsWith('.mp4'));
  assert.equal(inPlaceFiles.length, 1);
  const inPlaceFile = path.join(inPlaceFolder, inPlaceFiles[0]);
  const inPlaceLayout = verifyInPlaceMp4(preparedFile, inPlaceFile);
  const inPlaceFrames = frameRows(run(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-nostdin',
    '-i', inPlaceFile, '-an', '-threads', '1', '-f', 'framemd5', '-']).stdout);
  assert.deepEqual(inPlaceFrames, reference, 'in-place: decoded frames/timestamps differ');
  if (trial === 0) {
    for (const position of ['119.5', '0', '60.125', '0.5']) {
      const seek = input => frameRows(run(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-nostdin',
        '-ss', position, '-i', input, '-an', '-frames:v', '3', '-threads', '1', '-f', 'framemd5', '-']).stdout);
      const expected = seek(preparedFile);
      assert.equal(expected.length, 3);
      assert.deepEqual(seek(inPlaceFile), expected, `in-place seek at ${position}s`);
    }
  }
  const inPlaceLog = result.stderr.match(/\[in-place\] ok=1 mediaRead=(\d+) mediaWritten=(\d+) metadataWritten=(\d+) mediaBytes=(\d+)/);
  assert.ok(inPlaceLog, 'in-place finalization counters required');
  assert.equal(Number(inPlaceLog[1]), 0);
  assert.equal(Number(inPlaceLog[2]), 0);
  assert.ok(Number(inPlaceLog[3]) > 0);
  assert.equal(Number(inPlaceLog[4]), inPlaceLayout.mediaBytes);
  inPlaceRows.push({ trial, ...inPlaceLayout, saveMediaRead: 0, saveMediaWritten: 0,
    saveMetadataWritten: Number(inPlaceLog[3]) });
  for (const alignment of [4096, 65536]) {
    const folder = path.join(output, `prepared/aligned-${alignment}`);
    const files = fs.readdirSync(folder).filter(name => name.endsWith('.mp4'));
    assert.equal(files.length, 1);
    const file = path.join(folder, files[0]);
    const layout = verifyAlignedMp4(preparedFile, file);
    const frames = frameRows(run(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-nostdin',
      '-i', file, '-an', '-threads', '1', '-f', 'framemd5', '-']).stdout);
    assert.deepEqual(frames, reference, `aligned-${alignment}: decoded frames/timestamps differ`);
    if (trial === 0) {
      for (const position of ['119.5', '0', '60.125', '0.5']) {
        const seek = input => frameRows(run(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-nostdin',
          '-ss', position, '-i', input, '-an', '-frames:v', '3', '-threads', '1', '-f', 'framemd5', '-']).stdout);
        const expected = seek(preparedFile);
        assert.equal(expected.length, 3, 'seek fixture must return frames');
        assert.deepEqual(seek(file), expected, `aligned-${alignment}: seek at ${position}s differs`);
      }
    }
    const match = result.stderr.match(new RegExp(`\\[aligned-payload\\] alignment=${alignment} samples=(\\d+) mediaBytes=(\\d+) paddingBytes=(\\d+) candidateBytes=(\\d+)`));
    assert.ok(match, 'native planner must report geometric coverage');
    assert.equal(Number(match[2]), layout.mediaBytes);
    assert.equal(Number(match[3]), layout.paddingBytes);
    assert.ok(Number(match[4]) > 0 && Number(match[4]) <= layout.mediaBytes);
    alignedRows.push({ trial, alignment, ...layout, candidateBytes: Number(match[4]) });
  }
}
fs.writeFileSync(path.join(scratch, 'summary.json'), JSON.stringify(rows, null, 2));
fs.writeFileSync(path.join(scratch, 'aligned-summary.json'), JSON.stringify(alignedRows, null, 2));
console.table(rows);
console.table(alignedRows);
fs.writeFileSync(path.join(scratch, 'in-place-summary.json'), JSON.stringify(inPlaceRows, null, 2));
console.table(inPlaceRows);
console.log('PASS: in-place MP4s preserve sample/frame/seek parity with zero Save media reads/writes and same-file rename.');
console.log('PASS: byte-identical MP4s and 3600 identical decoded frames for legacy/prepared/mixed/extent-copy, three trials.');
console.log('PASS: aligned 4KiB/64KiB MP4s preserve every encoded sample, timing/index metadata and decoded frame; gaps are outside samples.');
console.log('PASS: native sealed-file clone-or-copy jobs retain full MP4 byte/frame parity. See extent.log for actual clone vs fallback coverage.');
console.log('Small synthetic, warm-cache comparison; not a gaming/physical-disk benchmark. Artifacts: ' + scratch);
