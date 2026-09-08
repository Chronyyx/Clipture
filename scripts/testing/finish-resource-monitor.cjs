const fs = require('node:fs');
const path = require('node:path');

module.exports = async function finishResourceMonitor(monitor, profile) {
  if (monitor.exitCode === null && monitor.signalCode === null) {
    await new Promise(resolve => {
      const timer = setTimeout(() => { monitor.kill(); resolve(); }, 15000);
      monitor.once('exit', () => { clearTimeout(timer); resolve(); });
    });
  }
  try {
    const text = fs.readFileSync(path.join(profile,'app-exit-resources.json'),'utf8').replace(/^\uFEFF/,'');
    return JSON.parse(text);
  } catch { return {ok:false,error:'Resource monitor did not finish its post-exit orphan check.'}; }
};
