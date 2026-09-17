'use strict';
// Read-only analysis of raw engine stderr or Tauri's chunk-prefixed stderr.
const fs = require('node:fs');

function summarize(text, from = -Infinity, until = Infinity) {
  if (!(from <= until)) throw Error('Invalid UTC interval');
  const normalized = text.replace(/\u001b\[[0-9;]*m/g, '')
    .replace(/\d{4}-\d{2}-\d{2}T[\d:.]+Z\s+WARN\s+clipture_engine:\s?/g, '')
    .replace(/[\r\n]/g, '');
  const entries = normalized.split('[pipeline-trace]').slice(1);
  const records = [];
  let rejected = 0, excluded = 0;
  for (const entry of entries) {
    const match = entry.match(/^\s*(\{[^{}]*\})/);
    try {
      if (!match) throw Error('Non-JSON trace');
      const value = JSON.parse(match[1]);
      if (typeof value.stage !== 'string' || !Number.isFinite(value.utcMs) ||
          !['cpu', 'gpu-sampled', 'gpu-health'].includes(value.kind)) throw Error('Invalid record');
      const required = value.kind === 'gpu-health'
        ? ['completed', 'skipped', 'pending', 'oldestPendingMs']
        : ['windowMs', 'count', 'avgMs', 'maxMs', 'over1Ms', 'over5Ms', 'over10Ms'];
      if (required.some(key => !Number.isFinite(value[key]) || value[key] < 0) ||
          (value.kind !== 'gpu-health' && value.count === 0)) throw Error('Invalid counters');
      if (value.utcMs < from || value.utcMs > until) { ++excluded; continue; }
      records.push(value);
    } catch { ++rejected; }
  }
  const groups = new Map();
  for (const r of records) {
    const key = `${r.stage}/${r.kind}`;
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(r);
  }
  const stages = [...groups].map(([stage, rows]) => {
    if (rows[0].kind === 'gpu-health') return { stage, records: rows.length,
      maxPending: Math.max(...rows.map(r => r.pending)),
      maxOldestPendingMs: Math.max(...rows.map(r => r.oldestPendingMs)),
      // Cumulative counters can reset on device/thread restart. Preserve raw rows.
      lastCompleted: rows.at(-1).completed, lastSkipped: rows.at(-1).skipped };
    const count = rows.reduce((n, r) => n + r.count, 0);
    return { stage, records: rows.length, count,
      weightedAvgMs: rows.reduce((n, r) => n + r.avgMs * r.count, 0) / count,
      maxMs: Math.max(...rows.map(r => r.maxMs)),
      over1Ms: rows.reduce((n, r) => n + r.over1Ms, 0),
      over5Ms: rows.reduce((n, r) => n + r.over5Ms, 0),
      over10Ms: rows.reduce((n, r) => n + r.over10Ms, 0) };
  });
  return { note: 'UTC selects interval-end observations, not GPU issue times. GPU samples exclude pre-marker queue delay; no p95 or throughput ceiling is inferred. Rejected includes unavailable-query messages and malformed/chunk-corrupted records.',
    markers: entries.length, rejected, excluded, stages, records };
}

if (require.main === module) {
  try {
    const [input, output, from, until] = process.argv.slice(2);
    if (!input || !output) throw Error('Usage: node summarize-pipeline-trace.cjs <engine.log> <new-report.json> [from-UTC] [until-UTC]');
    const result = summarize(fs.readFileSync(input, 'utf8'), from ? Date.parse(from) : -Infinity,
      until ? Date.parse(until) : Infinity);
    fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n', { flag: 'wx' });
    console.log(JSON.stringify({ ...result, records: result.records.length }, null, 2));
    if (result.rejected || !result.records.length) process.exitCode = 2;
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
module.exports = { summarize };
