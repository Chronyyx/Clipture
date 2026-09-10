const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');

module.exports = function verifyInPlaceAudio({ root, ffmpeg, run, sourceHashes }) {
  const rows = text => text.split(/\r?\n/).filter(line => line && !line.startsWith('#'));
  for (const [name, seconds] of [['first', 120], ['second', 10]]) {
    const folder = path.join(root, name);
    const files = ['compact', 'in-place'].map(mode => {
      const directory = path.join(folder, mode);
      const found = fs.readdirSync(directory).filter(file => file.endsWith('.mp4'));
      assert.equal(found.length, 1);
      return path.join(directory, found[0]);
    });
    const video = files.map(file => rows(run(ffmpeg, ['-v', 'error', '-nostdin', '-i', file,
      '-map', '0:v:0', '-an', '-threads', '1', '-f', 'framemd5', '-']).stdout));
    assert.equal(video[0].length, seconds * 30);
    assert.deepEqual(video[1], video[0], name + ': in-place video frames/timestamps');
    const expected = name === 'first' ? sourceHashes.slice(45, 3645)
      : [...sourceHashes.slice(3645), ...sourceHashes.slice(0, 285)];
    assert.deepEqual(video[1].map(line => line.split(',').at(-1).trim()), expected, name + ': visible non-overlapping interval');
    for (const track of [0, 1]) {
      const audio = files.map(file => run(ffmpeg, ['-v', 'error', '-nostdin', '-i', file,
        '-map', `0:a:${track}`, '-vn', '-c:a', 'pcm_s16le', '-f', 'hash', '-hash', 'sha256', '-']).stdout);
      assert.match(audio[0], /^SHA256=\w+/);
      assert.equal(audio[1], audio[0], `${name}: audio ${track} PCM hash`);
    }
    for (const position of name === 'first' ? ['0', '60.125', '119.5'] : ['0', '5', '9.5']) {
      for (const stream of ['v:0', 'a:0', 'a:1']) {
        const hashes = files.map(file => run(ffmpeg, ['-v', 'error', '-nostdin', '-ss', position,
          '-i', file, '-t', '0.2', '-map', `0:${stream}`, '-f', 'hash', '-hash', 'sha256', '-']).stdout);
        assert.equal(hashes[1], hashes[0], `${name}: ${stream} seek at ${position}s`);
      }
    }
    const io = JSON.parse(fs.readFileSync(path.join(folder, 'io.json')));
    assert.equal(io.seconds, seconds);
    assert.ok(io.supplementalMediaBytes < 512 * 1024);
    console.log(`PASS: ${seconds}s in-place A/V, 2 AAC tracks, frame/PCM/seek parity; supplemental media ${io.supplementalMediaBytes} bytes.`);
  }
  const rolling = path.join(root, '..', 'rolling');
  const rollingFiles = ['compact', 'in-place'].map(mode => {
    const directory = path.join(rolling, mode);
    return path.join(directory, fs.readdirSync(directory).find(name => name.endsWith('.mp4')));
  });
  const rollingFrames = rollingFiles.map(file => rows(run(ffmpeg, ['-v', 'error', '-nostdin', '-i', file,
    '-map', '0:v:0', '-an', '-threads', '1', '-f', 'framemd5', '-']).stdout));
  assert.deepEqual(rollingFrames[1], rollingFrames[0], 'reused rolling offsets preserve frame/timestamp parity');
  assert.equal(rollingFrames[1].length, 450);
  assert.deepEqual(rollingFrames[1].map(line => line.split(',').at(-1).trim()), sourceHashes.slice(-450));
  for (const position of ['0', '5', '14.5']) {
    const seek = file => run(ffmpeg, ['-v', 'error', '-nostdin', '-ss', position, '-i', file,
      '-t', '0.2', '-map', '0:v:0', '-f', 'hash', '-hash', 'sha256', '-']).stdout;
    assert.equal(seek(rollingFiles[1]), seek(rollingFiles[0]), 'seek across reused file offsets');
  }
  assert.ok(fs.statSync(rollingFiles[1]).size < 2 * 1024 * 1024);
  console.log('PASS: 366s recording into reused bounded storage, 15s final clip, frame and seek parity.');
};
