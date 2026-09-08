const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { imports, assertStandaloneRuntime } = require('./windows-runtime-imports.cjs');

function fixture(dll, delayed = false, pe32 = false) {
  const bytes = Buffer.alloc(2048), pe = 128, optional = pe + 24;
  bytes.writeUInt16LE(0x5a4d, 0); bytes.writeUInt32LE(pe, 0x3c);
  bytes.writeUInt32LE(0x4550, pe); bytes.writeUInt16LE(1, pe + 6);
  const optionalSize = pe32 ? 224 : 240;
  bytes.writeUInt16LE(optionalSize, pe + 20);
  bytes.writeUInt16LE(pe32 ? 0x10b : 0x20b, optional);
  const directories = optional + (pe32 ? 96 : 112);
  bytes.writeUInt32LE(16, directories - 4);
  const entry = directories + (delayed ? 13 : 1) * 8;
  bytes.writeUInt32LE(4096, entry); bytes.writeUInt32LE(delayed ? 64 : 40, entry + 4);
  const section = optional + optionalSize;
  bytes.writeUInt32LE(1536, section + 8); bytes.writeUInt32LE(4096, section + 12);
  bytes.writeUInt32LE(1536, section + 16); bytes.writeUInt32LE(512, section + 20);
  if (delayed) bytes.writeUInt32LE(1, 512);
  bytes.writeUInt32LE(4608, 512 + (delayed ? 4 : 12));
  bytes.write(dll, 1024, 'ascii');
  return bytes;
}

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'clipture-pe-test-'));
const file = path.join(directory, 'fixture.exe');
try {
  for (const delayed of [false, true]) {
    for (const pe32 of [false, true]) {
      for (const dll of ['KERNEL32.dll', 'MSVCP140_ATOMIC_WAIT.dll', 'VCRUNTIME140_1.dll', 'ucrtbased.dll']) {
        const bytes = fixture(dll, delayed, pe32);
        assert.deepEqual(imports(bytes), [dll]);
        fs.writeFileSync(file, bytes);
        if (dll === 'KERNEL32.dll') assert.deepEqual(assertStandaloneRuntime(file), [dll]);
        else assert.throws(() => assertStandaloneRuntime(file), /separately installed VC runtime/);
      }
    }
  }
  assert.throws(() => imports(Buffer.alloc(30)), /Malformed/);
  assert.throws(() => imports(fixture('KERNEL32.dll').subarray(0, 600)), /Malformed/);
  const broken = fixture('KERNEL32.dll'); broken.writeUInt32LE(0xfffffff0, 524);
  assert.throws(() => imports(broken), /Malformed/);
  console.log('Windows PE runtime import checks passed (normal/delay, PE32/PE32+, malformed images).');
} finally {
  fs.unlinkSync(file);
  fs.rmdirSync(directory);
}
