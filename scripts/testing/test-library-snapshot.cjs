// Pure feature-state regression test: no browser, desktop host or user data.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');
const file = path.resolve(__dirname, '../../src/renderer/features/library/librarySnapshot.ts');
const source = ts.transpileModule(fs.readFileSync(file, 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 }
}).outputText;
const exportsObject = {};
vm.runInNewContext(source, { exports: exportsObject });
const { LibrarySnapshot } = exportsObject;
const clip = (id, title = id) => ({ id, title });
const ids = clips => Array.from(clips, value => value.id);

const library = new LibrarySnapshot();
const initial = library.begin();
library.upsert(clip('new'));
assert.deepEqual(ids(library.complete(initial, [clip('old')])), ['new', 'old']);
const older = library.begin();
library.upsert(clip('new', 'renamed'));
const newer = library.begin();
assert.equal(library.complete(older, []), undefined);
const merged = library.complete(newer, [clip('new', 'stale'), clip('old')]);
assert.equal(merged[0].title, 'renamed');
assert.deepEqual(ids(merged), ['new', 'old']);
const deletion = library.begin();
assert.deepEqual(ids(library.complete(deletion, [clip('old')])), ['old']);
const closing = library.begin();
library.invalidate();
assert.equal(library.complete(closing, []), undefined);
console.log('Library snapshot races passed: startup event, stale response, deduplication, deletion and disposal.');
