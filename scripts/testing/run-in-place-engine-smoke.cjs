// Explicit hardware capture into a fresh workspace directory. No daily profile,
// microphone, hotkey registration, UI, installation or process-name cleanup.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { spawn, spawnSync } = require('node:child_process');
assert.ok(process.argv.includes('--record-primary-display'), 'Pass --record-primary-display to authorize this capture test.');
const root = path.resolve(__dirname, '../..');
fs.mkdirSync(path.join(root, '.cache'), { recursive: true });
const scratch = fs.mkdtempSync(path.join(root, '.cache/in-place-engine-'));
const clips = path.join(scratch, 'clips');
fs.mkdirSync(clips);
const child = spawn(path.join(root, 'build/engine/Release/clipture_engine.exe'), [], {
  windowsHide: true, cwd: root, stdio: ['pipe', 'pipe', 'pipe'],
  env: { ...process.env, TEMP: scratch, TMP: scratch, LOCALAPPDATA: scratch, APPDATA: scratch }
});
let sequence = 0, incoming = '', logs = '';
const pending = new Map();
const exited = new Promise(resolve => child.once('exit', resolve));
child.stderr.on('data', data => { logs = (logs + data).slice(-8 * 1024 * 1024); });
child.stdout.on('data', data => {
  incoming += data;
  while (incoming.includes('\n')) {
    const index = incoming.indexOf('\n'), line = incoming.slice(0, index);
    incoming = incoming.slice(index + 1);
    let response;
    try { response = JSON.parse(line); } catch { continue; }
    const call = pending.get(response.id);
    if (!call) continue;
    pending.delete(response.id); clearTimeout(call.timer);
    if (response.error) call.reject(new Error(response.error)); else call.resolve(response.payload);
  }
});
child.on('error', error => { for (const call of pending.values()) { clearTimeout(call.timer); call.reject(error); } pending.clear(); });
const request = (type, fields = {}) => new Promise((resolve, reject) => {
  const id = ++sequence;
  const timer = setTimeout(() => { pending.delete(id); reject(new Error(type + ' timed out')); }, 30000);
  pending.set(id, { resolve, reject, timer });
  child.stdin.write(JSON.stringify({ id, type, ...fields }) + '\n');
});
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const config = { fps: 30, bitrateMbps: 4, clipLengthSeconds: 5, targetWidth: 640, targetHeight: 360,
  saveFolder: clips, saveInPlace: true, includeMixedAudio: false, includeSystemAudio: false,
  includeMicrophoneAudio: false, captureGameAudio: false, captureForegroundSystemAudio: false };
const results = [];
const watchdog = setTimeout(() => child.kill(), 90000);
(async () => {
  try {
    await request('configure', config);
    await delay(8000);
    const diagnostics = await request('getDiagnostics');
    assert.ok(diagnostics.bufferedVideoPackets > 0, 'hardware must produce encoded video');
    const save = async () => {
      const result = await request('saveClip', { durationSeconds: 5, saveFolder: clips, analyzeIo: true });
      assert.ok(result.ok && result.clip, JSON.stringify(result));
      results.push(result.clip);
      console.log(JSON.stringify({ clip: results.length, seconds: result.clip.durationSeconds, message: result.message }));
      return result.clip;
    };
    assert.equal((await save()).durationSeconds, 5);
    await delay(2000);
    const second = await save();
    assert.ok(second.durationSeconds >= 1 && second.durationSeconds <= 3, 'second save must contain only new footage');
    await delay(1000);
    const blocked = path.join(scratch, 'not-a-folder'); fs.writeFileSync(blocked, 'fixture');
    let rejected = false;
    const failureStarted = Date.now();
    try { rejected = !(await request('saveClip', { durationSeconds: 5, saveFolder: blocked })).ok; }
    catch (error) { assert.doesNotMatch(error.message, /timed out/); rejected = true; }
    assert.ok(rejected, 'invalid save destination must fail');
    assert.ok(Date.now() - failureStarted < 5000, 'native errors must return valid JSON without a timeout');
    const recovered = await save();
    assert.ok(recovered.durationSeconds >= 1 && recovered.durationSeconds <= 3, 'failed save must not consume its window');
    await request('configure', { ...config, saveInPlace: false });
    assert.equal((await save()).durationSeconds, 5, 'opt-out restores full overlapping window');
    assert.equal(new Set(results.map(clip => clip.filePath)).size, results.length, 'rapid saves must not overwrite an earlier clip');
    const ffmpeg = require('ffmpeg-static');
    for (const clip of results) {
      const decoded = spawnSync(ffmpeg, ['-v', 'error', '-nostdin', '-i', clip.filePath, '-f', 'null', '-'],
        { windowsHide: true, encoding: 'utf8', timeout: 30000 });
      assert.equal(decoded.status, 0, decoded.stderr);
    }
    assert.ok((logs.match(/\[in-place\] ok=1/g) || []).length >= 3, 'native saves must actually finalize in place');
    fs.writeFileSync(path.join(scratch, 'result.json'), JSON.stringify({ ok: true, clips: results }, null, 2));
    console.log('PASS: live isolated capture, consume-on-save, failed-save retry, opt-out and decode. ' + scratch);
  } finally {
    child.stdin.end();
    const shutdown = setTimeout(() => child.kill(), 10000);
    await exited; clearTimeout(shutdown); clearTimeout(watchdog);
    fs.writeFileSync(path.join(scratch, 'engine.log'), logs);
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
