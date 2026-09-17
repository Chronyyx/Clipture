'use strict';
// Synthetic encoder throughput probe, NOT a Clipture/CS2 capture benchmark.
// Does not change settings or start another capture. Writes only its JSON report.
const fs = require('node:fs');
const path = require('node:path');
const { randomFillSync } = require('node:crypto');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { performance } = require('node:perf_hooks');
const root = path.resolve(__dirname, '../..');

function makeFrames(format, complex) {
  const pixels = 2560 * 1440;
  return Array.from({ length: complex ? 4 : 1 }, () => {
    const frame = Buffer.alloc(format === 'bgra' ? pixels * 4 : pixels * 3 / 2);
    if (complex) randomFillSync(frame);
    else frame.fill(96);
    if (format === 'bgra') {
      for (let i = 3; i < frame.length; i += 4) frame[i] = 255;
    } else {
      frame.fill(128, pixels); // Neutral chroma; changing luma is deliberate stress.
    }
    return frame;
  });
}

async function run(format, complex, repetition) {
  const frames = makeFrames(format, complex);
  const args = ['-hide_banner', '-nostats', '-loglevel', 'error',
    '-f', 'rawvideo', '-pix_fmt', format, '-s:v', '2560x1440', '-r', '60', '-i', 'pipe:0',
    '-frames:v', '360', '-an', '-c:v', 'h264_nvenc', '-preset', 'p3', '-tune', 'll',
    '-rc', 'cbr', '-b:v', '50M', '-maxrate', '50M', '-bufsize', '50M', '-g', '120',
    '-bf', '0', '-rc-lookahead', '0', '-spatial-aq', '0', '-temporal-aq', '0',
    '-multipass', 'disabled', '-progress', 'pipe:1', '-f', 'null', '-'];
  const started = performance.now();
  const child = spawn(path.join(root, 'node_modules/ffmpeg-static/ffmpeg.exe'), args,
    { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
  let progress = '', error = '';
  child.stdout.on('data', data => { progress += data; });
  child.stderr.on('data', data => { error += data; });
  const closed = once(child, 'close');
  const timer = setTimeout(() => child.kill(), 30000);
  child.stdin.on('error', () => {});
  try {
    for (let i = 0; i < 360; ++i) {
      if (!child.stdin.write(frames[i % frames.length])) await once(child.stdin, 'drain');
    }
    child.stdin.end();
    const [code] = await closed;
    const seconds = (performance.now() - started) / 1000;
    if (code !== 0) throw new Error(error || `FFmpeg exited ${code}`);
    const count = Number([...progress.matchAll(/^frame=(\d+)/gm)].at(-1)?.[1]);
    if (count !== 360) throw new Error(`Expected 360 frames; got ${count}`);
    return { format, content: complex ? 'changing-grain' : 'flat', repetition,
      frames: count, wallSeconds: seconds, endToEndFps: count / seconds };
  } finally {
    clearTimeout(timer);
    if (child.exitCode === null) child.kill();
  }
}

async function main() {
  const output = process.argv[2];
  if (!output || process.argv.includes('--help')) {
    console.log('node scripts/performance/nvenc-content-benchmark.cjs <new-report.json>\nUses GPU briefly; leave CS2 closed. No capture/settings changes.');
    return;
  }
  if (fs.existsSync(output)) throw new Error('Report already exists; choose a new path.');
  const report = { startedAt: new Date().toISOString(),
    caveats: ['Includes CPU pipe/upload, FFmpeg startup/drain; not pure NVENC latency.',
      'Not CS2, D3D capture, HDR tonemapping, or graphics-contention reproduction.',
      'Existing Clipture stays running; shared GPU activity can affect results.'], results: [] };
  for (const format of ['nv12', 'bgra']) {
    for (let repetition = 1; repetition <= 3; ++repetition) {
      for (const complex of repetition % 2 ? [false, true] : [true, false]) {
        const result = await run(format, complex, repetition);
        report.results.push(result);
        console.log(JSON.stringify(result));
      }
    }
  }
  fs.mkdirSync(path.dirname(path.resolve(output)), { recursive: true });
  fs.writeFileSync(output, JSON.stringify(report, null, 2) + '\n', { flag: 'wx' });
}
main().catch(error => { console.error(error); process.exitCode = 1; });
