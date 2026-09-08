// Read-only source architecture test; never imports the renderer or a host.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { builtinModules } = require('node:module');
const ts = require('typescript');
const root = path.resolve(__dirname, '../../src/renderer');
const normalize = value => value.split(path.sep).join('/');
const builtins = new Set(builtinModules.flatMap(name => [name, `node:${name}`]));

function files(directory) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap(entry => {
    const target = path.join(directory, entry.name);
    return entry.isDirectory() ? files(target) : /\.(tsx?|jsx?)$/.test(entry.name) ? [target] : [];
  });
}

function imports(file, text) {
  const ast = ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true);
  const result = [];
  function visit(node) {
    if ((ts.isImportDeclaration(node) || ts.isExportDeclaration(node)) && node.moduleSpecifier
      && ts.isStringLiteral(node.moduleSpecifier)) result.push(node.moduleSpecifier.text);
    if (ts.isCallExpression(node) && (node.expression.kind === ts.SyntaxKind.ImportKeyword
      || (ts.isIdentifier(node.expression) && node.expression.text === 'require'))
      && node.arguments[0] && ts.isStringLiteral(node.arguments[0])) result.push(node.arguments[0].text);
    ts.forEachChild(node, visit);
  }
  visit(ast);
  return result;
}

function violation(from, specifier, target) {
  const feature = from.match(/^features\/([^/]+)\//)?.[1];
  if (!from.startsWith('platform/') && (builtins.has(specifier)
    || /^(?:electron(?:\/|$)|@tauri-apps\/|node:)/.test(specifier))) return 'desktop transport outside platform/';
  const destinationFeature = target?.match(/^features\/([^/]+)(?:\/|$)/)?.[1];
  if (destinationFeature && destinationFeature !== feature
    && !new RegExp(`^features/${destinationFeature}(?:/index(?:\\.tsx?)?)?$`).test(target)) {
    return 'cross-feature deep import (use the feature root)';
  }
  if (from.startsWith('shared/') && /^(features|app)\//.test(target || '')) return 'shared code depends on a feature or app';
  if (feature && target?.startsWith('app/')) return 'feature depends on application composition';
  return undefined;
}

assert(violation('features/player/Player.tsx', '@tauri-apps/api/core'));
assert(violation('features/player/Player.tsx', 'node:fs'));
assert(violation('features/player/Player.tsx', '../library/Card', 'features/library/Card'));
assert.equal(violation('features/player/Player.tsx', '../library', 'features/library'), undefined);
assert.equal(violation('platform/tauri.ts', '@tauri-apps/api/core'), undefined);
assert.deepEqual(imports('fixture.ts', `export { x } from './x'; import('./y'); require('node:fs');`), ['./x', './y', 'node:fs']);

const errors = [];
const edges = new Map();
const sources = files(root);
for (const file of sources) {
  const from = normalize(path.relative(root, file));
  const sourceFeature = from.match(/^features\/([^/]+)\//)?.[1];
  for (const specifier of imports(file, fs.readFileSync(file, 'utf8'))) {
    const target = specifier.startsWith('.') ? normalize(path.relative(root, path.resolve(path.dirname(file), specifier))) : undefined;
    const reason = violation(from, specifier, target);
    if (reason) errors.push(`${from}: ${specifier}: ${reason}`);
    const destination = target?.match(/^features\/([^/]+)(?:\/|$)/)?.[1];
    if (sourceFeature && destination && sourceFeature !== destination) {
      if (!edges.has(sourceFeature)) edges.set(sourceFeature, new Set());
      edges.get(sourceFeature).add(destination);
    }
  }
}
function checkCycles(feature, trail = []) {
  if (trail.includes(feature)) { errors.push(`Feature cycle: ${[...trail, feature].join(' -> ')}`); return; }
  for (const next of edges.get(feature) || []) checkCycles(next, [...trail, feature]);
}
for (const feature of edges.keys()) checkCycles(feature);
assert.deepEqual(errors, [], errors.join('\n'));
console.log(`Renderer boundaries passed: ${sources.length} source files; no cross-feature deep imports, cycles, or desktop imports outside platform/.`);
