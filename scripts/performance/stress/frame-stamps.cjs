'use strict';
// Reads the stress window's frame counter stamp from every frame of a clip.
// Layout must match engine/tools/stress-window/FrameStamp.hpp.
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const STAMP = { x: 24, y: 24, cellWidth: 24, cellHeight: 32, referenceCells: 2, payloadBits: 32 };
STAMP.cells = STAMP.referenceCells + STAMP.payloadBits;
const MIN_CONTRAST = 64;
const ffmpeg = path.resolve(__dirname, '../../../node_modules/ffmpeg-static/ffmpeg.exe');

const stampCheck = counter => Math.imul(counter & 0xFFFFFF, 0x9E3779B1) >>> 24;

function videoSize(clip) {
  const probe = spawnSync(ffmpeg, ['-hide_banner', '-nostdin', '-i', clip], { encoding: 'utf8', windowsHide: true });
  const match = /Stream #\d+:\d+.*Video:.*?, (\d{2,5})x(\d{2,5})/.exec(probe.stderr);
  if (!match) throw Error(`No video stream in ${clip}`);
  return { width: Number(match[1]), height: Number(match[2]) };
}

// Decodes one stamp from a grayscale crop. Returns the counter or null.
function decodeStamp(pixels, cropWidth, scale, scaleY = scale) {
  const sample = cell => {
    const cx = Math.round((cell * STAMP.cellWidth + STAMP.cellWidth / 2) * scale);
    const cy = Math.round((STAMP.cellHeight / 2) * scaleY);
    const rx = Math.max(1, Math.floor(STAMP.cellWidth * scale / 4));
    const ry = Math.max(1, Math.floor(STAMP.cellHeight * scaleY / 4));
    let sum = 0, count = 0;
    for (let y = cy - ry; y <= cy + ry; ++y) {
      for (let x = cx - rx; x <= cx + rx; ++x) { sum += pixels[y * cropWidth + x]; ++count; }
    }
    return sum / count;
  };
  const white = sample(0), black = sample(1);
  if (white - black < MIN_CONTRAST) return null;
  const threshold = (white + black) / 2;
  let payload = 0;
  for (let bit = 0; bit < STAMP.payloadBits; ++bit) {
    payload = ((payload << 1) | (sample(STAMP.referenceCells + bit) > threshold ? 1 : 0)) >>> 0;
  }
  const counter = payload >>> 8;
  return (payload & 0xFF) === stampCheck(counter) ? counter : null;
}

// Returns [{ index, ptsSeconds, counter|null }] in presentation order. The
// window's buffer may be stretched to the screen, so X and Y scale separately.
function readFrameStamps(clip, bufferWidth, bufferHeight) {
  const video = videoSize(clip);
  const scale = video.width / bufferWidth, scaleY = video.height / bufferHeight;
  const crop = { x: Math.floor(STAMP.x * scale), y: Math.floor(STAMP.y * scaleY),
    width: Math.ceil(STAMP.cells * STAMP.cellWidth * scale), height: Math.ceil(STAMP.cellHeight * scaleY) };
  const decoded = spawnSync(ffmpeg, ['-hide_banner', '-v', 'info', '-nostdin', '-i', clip, '-an',
    '-vf', `crop=${crop.width}:${crop.height}:${crop.x}:${crop.y},showinfo`, '-fps_mode', 'passthrough',
    '-pix_fmt', 'gray', '-f', 'rawvideo', '-'], { windowsHide: true, maxBuffer: 1024 * 1024 * 1024 });
  if (decoded.status !== 0) throw Error(`ffmpeg failed: ${decoded.stderr.toString().slice(-2000)}`);
  const pts = [...decoded.stderr.toString().matchAll(/\bn:\s*(\d+)\s+pts:\s*-?\d+\s+pts_time:\s*(-?[\d.]+)/g)]
    .map(match => Number(match[2]));
  const frameBytes = crop.width * crop.height;
  const count = Math.floor(decoded.stdout.length / frameBytes);
  if (pts.length !== count) throw Error(`Decoded ${count} frames but ${pts.length} timestamps`);
  const stamps = [];
  for (let index = 0; index < count; ++index) {
    const pixels = decoded.stdout.subarray(index * frameBytes, (index + 1) * frameBytes);
    stamps.push({ index, ptsSeconds: pts[index], counter: decodeStamp(pixels, crop.width, scale, scaleY) });
  }
  return { video, scale, stamps };
}

module.exports = { STAMP, stampCheck, decodeStamp, readFrameStamps };
