'use strict';
// Build a separate runtime/profile. Never overwrite installed binaries/settings.
const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');
const root = path.resolve(__dirname, '../..');
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');

function main() {
  const reportPath = path.resolve(process.argv[2] || '');
  const report = JSON.parse(fs.readFileSync(reportPath, 'utf8'));
  const engine = path.join(root, 'build/engine/Release/clipture_engine.exe');
  if (!report.complete || report.expectedTrials !== 6 || report.trials.length !== 6 ||
      report.trials.some(t => !t.passed) || report.sha256 !== hash(engine)) throw Error('Complete matching compatibility report required.');
  const installed = path.join(process.env.ProgramFiles, 'Clipture');
  const sourceSettings = path.join(process.env.APPDATA, 'Clipture/data/settings.json');
  const settings = JSON.parse(fs.readFileSync(sourceSettings, 'utf8'));
  const base = path.join(root, '.cache/cs2-perf');
  const directory = fs.mkdtempSync(path.join(base, 'fresh-conversion-app-'));
  const profile = path.join(directory, 'profile');
  fs.mkdirSync(profile);
  const files = ['clipture.exe', 'clipture_engine.exe', 'ffmpeg.exe', 'assets/default.mp3', 'assets/option2.wav'];
  const hashes = {};
  for (const file of files) {
    const destination = path.join(directory, file);
    fs.mkdirSync(path.dirname(destination), { recursive: true });
    fs.copyFileSync(file === 'clipture_engine.exe' ? engine : path.join(installed, file), destination, fs.constants.COPYFILE_EXCL);
    hashes[file] = hash(destination);
  }
  settings.fps = 120;
  settings.saveFolder = path.join(profile, 'clips');
  const settingsFile = path.join(profile, 'settings.json');
  fs.writeFileSync(settingsFile, JSON.stringify(settings, null, 2), { flag: 'wx' });
  const manifest = { directory, profile, files: hashes, validationReport: reportPath,
    sourceSettingsHash: hash(sourceSettings), settingsHash: hash(settingsFile),
    note: 'Isolated copy of current settings; 120 FPS and a private clip folder. No daily settings/startup registration changes.' };
  fs.writeFileSync(path.join(directory, 'candidate.json'), JSON.stringify(manifest, null, 2), { flag: 'wx' });
  console.log(JSON.stringify(manifest, null, 2));
}
if (require.main === module) main();
