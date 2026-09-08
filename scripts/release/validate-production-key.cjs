// Read-only release guard. Never reads a private key or signing environment.
const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');
const developmentFingerprint = 'ae4b854c2cd0271d372f6b344ecf016c5af219ff4ce4843a29f64d3ee5e62f74';

function decodeBase64(value) {
  if (typeof value !== 'string' || !value.length || value.length > 4096 ||
      !/^[A-Za-z0-9+/]+={0,2}$/.test(value)) throw new Error('Invalid updater public-key encoding.');
  const bytes = Buffer.from(value, 'base64');
  if (bytes.toString('base64') !== value) throw new Error('Non-canonical updater public-key encoding.');
  return bytes;
}

function validateProductionKey(config) {
  const encoded = config?.plugins?.updater?.pubkey;
  const lines = decodeBase64(encoded).toString('utf8').trim().split(/\r?\n/);
  if (lines.length !== 2 || !lines[0].startsWith('untrusted comment:')) {
    throw new Error('Expected a complete Minisign public-key envelope.');
  }
  const key = decodeBase64(lines[1]);
  if (key.length !== 42 || key.subarray(0, 2).toString('ascii') !== 'Ed') {
    throw new Error('Invalid Minisign public-key payload.');
  }
  const fingerprint = createHash('sha256').update(key).digest('hex');
  if (fingerprint === developmentFingerprint) {
    throw new Error('Release blocked: replace the local development verification key with the production public key and matching CI secrets.');
  }
  return fingerprint;
}

if (require.main === module) {
  const file = path.resolve(__dirname, '../../src-tauri/tauri.release.conf.json');
  try { validateProductionKey(JSON.parse(fs.readFileSync(file, 'utf8'))); }
  catch (error) { console.error(error.message); process.exitCode = 1; }
}
module.exports = { validateProductionKey };
