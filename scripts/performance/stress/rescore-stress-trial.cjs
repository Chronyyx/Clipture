'use strict';
// Re-scores saved trials after a scoring change: node rescore-stress-trial.cjs <trial dir>...
// Uses the stamps.json, frames.csv and summaries a trial left behind; no capture.
const fs = require('node:fs');
const path = require('node:path');
const { parseFrameLog, scoreTrial, windowCost } = require('./score-stress-trial.cjs');

for (const directory of process.argv.slice(2)) {
  const read = file => fs.readFileSync(path.join(directory, file), 'utf8');
  const report = JSON.parse(read('report.json'));
  const summary = { refreshHz: 0, ...JSON.parse(read('trial/summary.json')) };
  if (!summary.refreshHz && process.env.REFRESH_HZ) summary.refreshHz = Number(process.env.REFRESH_HZ);
  report.score = scoreTrial({ stamps: JSON.parse(read('stamps.json')), frames: parseFrameLog(read('trial/frames.csv')),
    summary, captureFps: report.fps });
  report.windowCost = windowCost(JSON.parse(read('solo/summary.json')), summary);
  fs.writeFileSync(path.join(directory, 'report.json'), JSON.stringify(report, null, 2));
  console.log(path.basename(directory), JSON.stringify(report.score.totals));
}
