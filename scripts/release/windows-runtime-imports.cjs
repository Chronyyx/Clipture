// Read-only PE import inspection. No executable is loaded or run.
const fs = require('node:fs');

function imports(bytes) {
  const fail = () => { throw new Error('Malformed or unsupported PE image'); };
  const u16 = at => at >= 0 && at + 2 <= bytes.length ? bytes.readUInt16LE(at) : fail();
  const u32 = at => at >= 0 && at + 4 <= bytes.length ? bytes.readUInt32LE(at) : fail();
  if (u16(0) !== 0x5a4d) fail();
  const pe = u32(0x3c);
  if (u32(pe) !== 0x4550) fail();
  const optional = pe + 24, magic = u16(optional);
  if (magic !== 0x20b && magic !== 0x10b) fail();
  const directories = optional + (magic === 0x20b ? 112 : 96);
  const directoryCount = u32(directories - 4);
  const sections = optional + u16(pe + 20), count = u16(pe + 6);
  const offset = rva => {
    for (let index = 0; index < count; index++) {
      const section = sections + index * 40;
      const address = u32(section + 12), size = u32(section + 16);
      if (rva >= address && rva - address < size) {
        const result = u32(section + 20) + rva - address;
        if (result < bytes.length) return result;
      }
    }
    return fail();
  };
  const name = rva => {
    const start = offset(rva), end = bytes.indexOf(0, start);
    if (end < start || end - start > 260) fail();
    return bytes.toString('ascii', start, end);
  };
  const result = [];
  for (const [directory, stride, nameField] of [[1, 20, 12], [13, 32, 4]]) {
    if (directory >= directoryCount) continue;
    const rva = u32(directories + directory * 8);
    const size = u32(directories + directory * 8 + 4);
    if (!rva) continue;
    const start = offset(rva);
    let terminated = false;
    for (let relative = 0; relative + stride <= size; relative += stride) {
      const descriptor = start + relative, library = u32(descriptor + nameField);
      if (!library) { terminated = true; break; }
      // Modern delay import descriptors store RVAs, not absolute pointers.
      if (directory === 13 && u32(descriptor) !== 1) fail();
      result.push(name(library));
    }
    if (!terminated) fail();
  }
  return [...new Set(result)];
}

function assertStandaloneRuntime(file) {
  const libraries = imports(fs.readFileSync(file));
  const missingOnCleanWindows = libraries.filter(name =>
    /^(?:msvcp\d+\w*|vcruntime\d+\w*|concrt\d+\w*|vcomp\d+\w*|ucrtbased)\.dll$/i.test(name));
  if (missingOnCleanWindows.length) {
    throw new Error(`${file} requires separately installed VC runtime DLLs: ${missingOnCleanWindows.join(', ')}`);
  }
  return libraries;
}

module.exports = { imports, assertStandaloneRuntime };
if (require.main === module) {
  if (process.argv.length < 3) throw new Error('Provide one or more Windows executable paths');
  for (const file of process.argv.slice(2)) {
    console.log(JSON.stringify({ file, imports: assertStandaloneRuntime(file) }));
  }
}
