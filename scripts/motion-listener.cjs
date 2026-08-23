const fs = require('fs');
const path = require('path');

const logPath = path.join(__dirname, '..', 'motion-trace.log');

console.log('========================================================================');
console.log(' Clipture Live Motion Repeat Listener Active');
console.log(' Monitoring active gameplay stream for motion duplicate events...');
console.log(` Watching log: ${logPath}`);
console.log('========================================================================\n');

let lastSize = 0;
try {
  if (fs.existsSync(logPath)) {
    lastSize = fs.statSync(logPath).size;
  }
} catch {}

function checkNewEvents() {
  try {
    if (!fs.existsSync(logPath)) return;
    const currentSize = fs.statSync(logPath).size;
    if (currentSize <= lastSize) return;

    const fd = fs.openSync(logPath, 'r');
    const buffer = Buffer.alloc(currentSize - lastSize);
    fs.readSync(fd, buffer, 0, buffer.length, lastSize);
    fs.closeSync(fd);
    lastSize = currentSize;

    const lines = buffer.toString('utf8').split('\n').filter(Boolean);
    for (const line of lines) {
      try {
        const event = JSON.parse(line);
        console.log(`\n🔴 [MOTION REPEAT DETECTED] at ${event.timestamp}`);
        console.log(`   Repeated: ${event.recentMotionFramesRepeated} / ${event.recentMotionFramesTotal} frames (${event.recentMotionRepeatRatioPercent.toFixed(2)}%)`);
        console.log(`   Rates: Fresh=${event.fpsRates?.freshPublishedFps?.toFixed(1)} FPS, EncIn=${event.fpsRates?.encoderInputFps?.toFixed(1)} FPS, EncOut=${event.fpsRates?.encoderOutputFps?.toFixed(1)} FPS`);
        console.log(`   Source Interval: p50=${event.sourceIntervalMs?.p50}ms, p95=${event.sourceIntervalMs?.p95}ms, max=${event.sourceIntervalMs?.max}ms`);
        console.log(`   Scheduler Wake Lateness: p50=${event.schedulerWakeLatenessMs?.p50}ms, p95=${event.schedulerWakeLatenessMs?.p95}ms, max=${event.schedulerWakeLatenessMs?.max}ms`);
        console.log(`   NVENC Call Latency: avg=${event.nvencCallLatencyMs?.avg}ms, max=${event.nvencCallLatencyMs?.max}ms`);
        console.log(`   Identified Bottleneck: ${event.bottleneck} | Reason: ${event.dominantDropReason}`);
        console.log('------------------------------------------------------------------------');
      } catch {}
    }
  } catch {}
}

setInterval(checkNewEvents, 250);
