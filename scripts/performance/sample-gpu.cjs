'use strict';
// Read-only GPU sampling for a manually driven CS2 trial. No game/capture changes.
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { once } = require('node:events');

async function main() {
  const seconds = Number(process.argv[2]);
  const destination = process.argv[3];
  if (!destination || !Number.isInteger(seconds) || seconds < 1 || seconds > 600) {
    console.log('node scripts/performance/sample-gpu.cjs <seconds:1-600> <new-report.json>');
    return;
  }
  if (fs.existsSync(destination)) throw new Error('Report exists; choose a new path.');
  const fields = ['timestamp', 'index', 'name', 'driver_version', 'pstate',
    'utilization.gpu', 'utilization.encoder', 'utilization.decoder', 'memory.used',
    'memory.total', 'power.draw', 'temperature.gpu', 'clocks.current.graphics', 'clocks.current.memory'];
  const startedAt = new Date().toISOString();
  const child = spawn('nvidia-smi', [`--query-gpu=${fields.join(',')}`, '--format=csv,noheader,nounits', '-lms', '500'],
    { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  const samples = [];
  let pending = '', errors = '', endedByTimer = false;
  child.stdout.on('data', chunk => {
    pending += chunk;
    let newline;
    while ((newline = pending.indexOf('\n')) >= 0) {
      const line = pending.slice(0, newline).trim();
      pending = pending.slice(newline + 1);
      const values = line.split(',').map(value => value.trim());
      if (values.length === fields.length) samples.push({
        receivedAt: new Date().toISOString(),
        ...Object.fromEntries(fields.map((field, index) => [field, values[index]])) });
    }
  });
  child.stderr.on('data', chunk => { errors += chunk; });
  const timer = setTimeout(() => { endedByTimer = true; child.kill(); }, seconds * 1000);
  try {
    const [code] = await once(child, 'close');
    if (!endedByTimer || samples.length === 0) throw new Error(errors || `nvidia-smi stopped (${code}) without a complete sampling interval`);
    const summary = {};
    for (const field of fields.slice(5)) {
      const values = samples.map(sample => Number(sample[field])).filter(Number.isFinite).sort((a, b) => a - b);
      if (values.length) summary[field] = { min: values[0], max: values.at(-1),
        median: values[Math.floor(values.length / 2)] };
    }
    fs.mkdirSync(path.dirname(path.resolve(destination)), { recursive: true });
    fs.writeFileSync(destination, JSON.stringify({ startedAt, endedAt: new Date().toISOString(),
      requestedSeconds: seconds, note: 'GPU-wide vendor sampled utilization, not per-game GPU frametimes. GPU timestamp is local time; receivedAt is UTC.',
      summary, samples }, null, 2) + '\n', { flag: 'wx' });
    console.log(JSON.stringify({ destination, samples: samples.length, summary }, null, 2));
  } finally {
    clearTimeout(timer);
    if (child.exitCode === null) child.kill();
  }
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
