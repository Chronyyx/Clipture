'use strict';
const assert = require('node:assert/strict');
const { STAMP, stampCheck, decodeStamp } = require('./stress/frame-stamps.cjs');
const { parseFrameLog, scoreTrial, windowCost, compareReports } = require('./stress/score-stress-trial.cjs');

// Renders a stamp crop like the window does, at `scale`, with optional noise.
function renderStamp(counter, scale, { white = 235, black = 16, flipBit = null } = {}) {
  const width = Math.ceil(STAMP.cells * STAMP.cellWidth * scale), height = Math.ceil(STAMP.cellHeight * scale);
  const payload = (((counter & 0xFFFFFF) << 8) | stampCheck(counter)) >>> 0;
  const pixels = new Uint8Array(width * height);
  for (let y = 0; y < height; ++y) {
    for (let x = 0; x < width; ++x) {
      const cell = Math.floor(x / scale / STAMP.cellWidth);
      let bit = cell === 0 ? 1 : cell === 1 ? 0 : (payload >>> (31 - (cell - 2))) & 1;
      if (flipBit !== null && cell - 2 === flipBit) bit ^= 1;
      pixels[y * width + x] = bit ? white : black;
    }
  }
  return { pixels, width };
}

for (const [counter, scale] of [[1, 1], [123456, 1], [0xFFFFFF, 1], [4242, 0.75], [99, 1.5]]) {
  const { pixels, width } = renderStamp(counter, scale);
  assert.equal(decodeStamp(pixels, width, scale), counter, `counter ${counter} at scale ${scale}`);
}
const dim = renderStamp(777, 1, { white: 110, black: 20 });  // tone-mapped HDR white
assert.equal(decodeStamp(dim.pixels, dim.width, 1), 777);
const flipped = renderStamp(777, 1, { flipBit: 5 });
assert.equal(decodeStamp(flipped.pixels, flipped.width, 1), null, 'a misread bit fails the check');
const flat = renderStamp(777, 1, { white: 60, black: 40 });
assert.equal(decodeStamp(flat.pixels, flat.width, 1), null, 'no contrast means no stamp');

// Window presents frames 1-40 in a clear phase (index 1) and 41-50 in a load phase (index 2).
let csv = 'frame,phase,presentUs\n';
for (let frame = 1; frame <= 50; ++frame) csv += `${frame},${frame <= 40 ? 1 : 2},${frame * 1000}\n`;
const frames = parseFrameLog(csv);
const summary = { refreshHz: 240, phases: [{ index: 0, name: 'lead', layers: 0, fps: 0 },
  { index: 1, name: 'clear', layers: 0, fps: 1000 }, { index: 2, name: 'smoke', layers: 20, fps: 150 },
  { index: 3, name: 'tail', layers: 0, fps: 0 }] };
// Clip at 100 fps: clear frames advance every sample; the load phase freezes on 41 for three samples.
const counters = [2, 12, 22, 32, 41, 41, 41, 44, null, 50];
const stamps = counters.map((counter, index) => ({ index, ptsSeconds: index * 0.01 + 0.0005, counter }));
const score = scoreTrial({ stamps, frames, summary, captureFps: 100 });
assert.equal(score.undecodedFrames, 1);
const [clear, smoke] = score.phases;
assert.deepEqual([clear.name, clear.missedFrames, clear.repeats, clear.freshnessPct], ['clear', 0, 0, 100]);
assert.deepEqual([smoke.name, smoke.outputFrames, smoke.uniqueFrames, smoke.repeats], ['smoke', 5, 3, 2]);
// Frame 42 was 10 ms old at the first repeat (within 0 + 6.67 + 4.17 ms) and 20 ms old at the second.
assert.equal(smoke.missedFrames, 1);
// The window (150 fps) outran capture (100 fps) by over 25%, so both repeats are stutter.
assert.equal(smoke.stutterFrames, 2);
assert.equal(clear.stutterFrames, 0);
assert.equal(smoke.longestFreezeMs, 30);
assert.equal(smoke.lagMs.max > clear.lagMs.max, true, 'frozen frames reach the clip late');
assert.equal(score.totals.loadedMissedFrames, 1);

// Window at the capture rate: a repeat whose successor was only just submitted is sampling, not loss.
let aliasCsv = 'frame,phase,presentUs\n';
for (let frame = 1; frame <= 6; ++frame) aliasCsv += `${frame},1,${frame * 10000 + (frame === 3 ? 9000 : 0)}\n`;
const alias = scoreTrial({ stamps: [1, 2, 2, 4, 5].map((counter, index) => ({ index, ptsSeconds: (index + 1) * 0.01, counter })),
  frames: parseFrameLog(aliasCsv), summary: { refreshHz: 100, phases: [{ index: 1, name: 'clear', layers: 0, fps: 100 }] }, captureFps: 100 });
assert.deepEqual([alias.phases[0].repeats, alias.phases[0].missedFrames], [1, 0]);

const cost = windowCost({ phases: [{ index: 2, name: 'smoke', fps: 200, frames: 10 }] },
  { phases: [{ index: 2, name: 'smoke', fps: 150, frames: 10 }, { index: 3, name: 'tail', fps: 0, frames: 0 }] });
assert.deepEqual(cost, [{ index: 2, name: 'smoke', soloFps: 200, trialFps: 150, costPct: 25 }]);
const better = { score: { ...score, totals: { ...score.totals, missedFrames: 0 } }, windowCost: cost };
assert.equal(compareReports({ score, windowCost: cost }, better).missedFrames, -1);
console.log('Stress trial tests passed (stamp decode, HDR contrast, bit errors, freshness, freezes, cost).');
