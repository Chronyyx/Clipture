// Test-only loopback HTTPS origin, launched solely inside the identified VM.
const fs = require('node:fs');
const https = require('node:https');
const os = require('node:os');
const input = 'C:\\CliptureInput';
const token = process.argv[2];
if (os.userInfo().username !== 'WDAGUtilityAccount' || !token ||
    fs.readFileSync(`${input}\\probe-token.txt`, 'utf8').trim() !== token) {
  throw new Error('Updater origin refuses to run outside the identified Sandbox');
}
const payload = `${input}\\Tauri.exe`;
const entry = { url: 'https://localhost:18443/installer.exe',
  signature: fs.readFileSync(`${input}\\Tauri.exe.sig`, 'utf8').trim() };
const manifest = Buffer.from(JSON.stringify({ version: '1.4.3',
  notes: 'Local integration fixture only; not a public release.',
  pub_date: new Date().toISOString(),
  platforms: { 'windows-x86_64': entry, 'windows-x86_64-nsis': entry } }));
const server = https.createServer({ key: fs.readFileSync(`${input}\\updater-key.pem`),
  cert: fs.readFileSync(`${input}\\updater-cert.pem`) }, (request, response) => {
  if (request.method !== 'GET') { response.writeHead(405).end(); return; }
  if (request.url === '/latest.json') {
    response.writeHead(200, { 'content-type': 'application/json', 'content-length': manifest.length });
    response.end(manifest);
  } else if (request.url === '/installer.exe') {
    response.writeHead(200, { 'content-type': 'application/octet-stream', 'content-length': fs.statSync(payload).size });
    fs.createReadStream(payload).pipe(response);
  } else response.writeHead(404).end();
});
server.listen(18443, '127.0.0.1', () => {
  fs.writeFileSync('C:\\CliptureOutput\\updater-server-ready.json', JSON.stringify({ token, pid: process.pid }));
});
setTimeout(() => { server.close(); server.closeAllConnections(); }, 6 * 60 * 1000);
