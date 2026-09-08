const assert = require('node:assert/strict');
const config = { plugins: { updater: { pubkey: 'dW50cnVzdGVkIGNvbW1lbnQ6IG1pbmlzaW5pIHB1YmxpYyBrZXk6IDQyMjJBNjNDNUI1RDAzQQpSV1E2MExYRll5b2lCR0tseTN2UUVTV2hQN2dMaVpjbzhvLzM5dmJ1RVZaaWpZeGU1OElKWXhWcQo=' } } };
const { validateProductionKey } = require('./validate-production-key.cjs');
const envelope = bytes => ({ plugins: { updater: { pubkey:
  Buffer.from(`untrusted comment: synthetic test fixture\n${bytes.toString('base64')}\n`).toString('base64') } } });
// Synthetic PUBLIC payload, not a generated signing key or a production key.
const publicFixture = Buffer.alloc(42, 7); publicFixture.write('Ed');
assert.equal(validateProductionKey(envelope(publicFixture)).length, 64);
assert.throws(() => validateProductionKey({}), /encoding/);
assert.throws(() => validateProductionKey(envelope(Buffer.alloc(41))), /payload/);
assert.throws(() => validateProductionKey(config), /local development/);
const changedComment = JSON.parse(JSON.stringify(config));
const lines = Buffer.from(config.plugins.updater.pubkey, 'base64').toString('utf8').split('\n');
lines[0] = 'untrusted comment: this is not proof of production ownership';
changedComment.plugins.updater.pubkey = Buffer.from(lines.join('\n')).toString('base64');
assert.throws(() => validateProductionKey(changedComment), /local development/);
console.log('Production key guard: missing, malformed and development keys rejected.');
