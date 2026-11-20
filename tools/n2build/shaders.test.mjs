// Tests for the shader cook step of n2build (lib/shaders.mjs, lib/tint.mjs). Run: node --test tools/n2build/shaders.test.mjs
// The cook itself needs glslang, spirv-val, spirv-dis and spirv-cross (and Tint for WGSL); those tests skip when a tool is missing.
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import zlib from 'node:zlib';

import { cookShader, manifestBytes } from '../n2shader/lib/cook.mjs';
import { discoverTools } from '../n2shader/lib/tools.mjs';
import { MANIFEST_NAME, cookPack, writeZip } from './lib/cook.mjs';
import { BuildError } from './lib/report.mjs';
import {
  cookProjectShaders, findShaderSources, missingShaderTools, shaderTargetsFor, stripSource,
} from './lib/shaders.mjs';
import { DAWN_COMMIT, TINT_PINS, archiveUrl, cachedTint, extractTarGzEntry, installTint, sha256Of, tintKey, toolsFolder } from './lib/tint.mjs';

const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const fixtures = path.resolve(here, '..', '..', 'tests', 'renderer', 'fixtures', 'user-shaders');
const tmp = (prefix) => fs.mkdtempSync(path.join(os.tmpdir(), prefix));
const UUID = '11111111-2222-3333-4444-555555555555';

const tools = discoverTools();
const haveCore = Boolean(tools.glslang && tools.spirvVal && tools.spirvDis && tools.spirvCross);
const skipCore = !haveCore && 'glslang, spirv-val, spirv-dis or spirv-cross is not installed';
const skipTint = (skipCore || !tools.tint) && (skipCore || 'tint is not installed');

function write(file, content) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, content);
}

/** A project with the solid fixture as assets/fx/solid.n2shader and (unless noMeta) its .meta */
function makeProject({ noMeta = false, extra = {} } = {}) {
  const dir = tmp('n2build-shaders-');
  write(path.join(dir, 'project.n2proj'), JSON.stringify({ name: 'Game', projectId: '00000000-0000-0000-0000-000000000001' }));
  write(path.join(dir, 'assets', 'fx', 'solid.n2shader'), fs.readFileSync(path.join(fixtures, 'solid.n2shader')));
  if (!noMeta) write(path.join(dir, '.import', 'fx', 'solid.n2shader.meta'), JSON.stringify({ uuid: UUID, resourcePath: 'res://assets/fx/solid.n2shader', resourceType: 'Shader' }));
  for (const [name, content] of Object.entries(extra)) write(path.join(dir, name), content);
  return dir;
}

const names = (entries) => entries.map((entry) => entry.name);

test('shader payloads per package kind, and the descriptor override', () => {
  assert.deepEqual(shaderTargetsFor({ package: { kind: 'desktop' } }), ['vulkan', 'gl']);
  assert.deepEqual(shaderTargetsFor({ package: { kind: 'web' } }), ['webgpu']);
  assert.deepEqual(shaderTargetsFor({ package: { kind: 'android' } }), ['vulkan']);
  assert.deepEqual(shaderTargetsFor({ package: { kind: 'ios' } }), ['vulkan']);
  assert.deepEqual(shaderTargetsFor({ package: { kind: 'desktop' }, shaderTargets: ['gl'] }), ['gl']);
});

test('findShaderSources: only .n2shader under assets/, sorted', () => {
  const dir = makeProject({ extra: { 'assets/a.n2shader': 'x', 'assets/z/B.N2SHADER': 'x', 'assets/readme.txt': 'x', 'other/q.n2shader': 'x' } });
  assert.deepEqual(findShaderSources(dir).map((s) => s.name), ['assets/a.n2shader', 'assets/fx/solid.n2shader', 'assets/z/B.N2SHADER']);
  assert.deepEqual(findShaderSources(tmp('n2build-empty-')), []);
});

test('missingShaderTools: Tint is needed for WebGPU only', () => {
  const none = { glslang: null, spirvVal: null, spirvDis: null, spirvCross: null, tint: null };
  assert.equal(missingShaderTools(none, ['vulkan', 'gl']).length, 4);
  assert.equal(missingShaderTools(none, ['webgpu']).length, 5);
  const all = { glslang: 'g', spirvVal: 'v', spirvDis: 'd', spirvCross: 'c', tint: null };
  assert.deepEqual(missingShaderTools(all, ['vulkan']), []);
  assert.equal(missingShaderTools(all, ['webgpu']).length, 1);
});

test('stripSource keeps the line of a custom vertex stage (the engine tells it from the text)', () => {
  const text = '{ "n2shader": 1, "name": "V" }\n#stage fragment\nvoid main() {}\n#stage vertex\nvoid n2_vertex() {}\n';
  assert.equal(stripSource(text), '{ "n2shader": 1, "name": "V" }\n#stage fragment\n#stage vertex\n');
  assert.equal(stripSource('{ "n2shader": 1 }\n#stage fragment\nvoid main() {}\n'), '{ "n2shader": 1 }\n#stage fragment\n');
});

test('stripSource keeps the header and an empty fragment stage', () => {
  const text = fs.readFileSync(path.join(fixtures, 'solid.n2shader'), 'utf8');
  const stripped = stripSource(text);
  assert.ok(stripped.endsWith('#stage fragment\n'));
  assert.ok(stripped.startsWith('{ "n2shader": 1'));
  assert.ok(!stripped.includes('void main'));
  assert.equal(stripSource('no stage here'), 'no stage here');
});

test('a project without shaders costs nothing: no tools needed, no entries', () => {
  const dir = tmp('n2build-noshaders-');
  write(path.join(dir, 'project.n2proj'), '{}');
  const result = cookProjectShaders({ projectDir: dir, targets: ['webgpu'], tools: { glslang: null } });
  assert.deepEqual(result, { entries: [], replaced: new Map(), count: 0 });
});

test('missing tools and a missing .meta are named, before anything is cooked', () => {
  assert.throws(() => cookProjectShaders({ projectDir: makeProject(), targets: ['vulkan'], tools: { glslang: null, spirvVal: null, spirvDis: null, spirvCross: null } }),
    (error) => error instanceof BuildError && /glslangValidator/.test(error.message) && /1 shader/.test(error.message));
  const all = { glslang: 'g', spirvVal: 'v', spirvDis: 'd', spirvCross: 'c', tint: 't' };
  assert.throws(() => cookProjectShaders({ projectDir: makeProject({ noMeta: true }), targets: ['vulkan'], tools: all }),
    (error) => error instanceof BuildError && /no \.meta/.test(error.message) && /assets\/fx\/solid\.n2shader/.test(error.message));
});

test('a shader that does not cook fails the cook with its file and line', { skip: skipCore }, () => {
  const dir = makeProject({ extra: { 'assets/fx/solid.n2shader': '{ "n2shader": 1, "name": "Bad" }\n#stage fragment\nvoid main() { n2_FragColor = vec4(oops); }\n' } });
  assert.throws(() => cookProjectShaders({ projectDir: dir, targets: ['vulkan'], tools }),
    (error) => error instanceof BuildError && /assets\/fx\/solid\.n2shader did not cook/.test(error.message) && /assets\/fx\/solid\.n2shader:\d+/.test(error.message));
});

test('the cook writes what n2shader cook writes, under .import/shaders/<uuid>/, sorted', { skip: skipCore }, () => {
  const dir = makeProject();
  const result = cookProjectShaders({ projectDir: dir, targets: ['vulkan', 'gl'], tools });
  assert.deepEqual(names(result.entries), [
    `.import/shaders/${UUID}/manifest.json`, `.import/shaders/${UUID}/shader.gl.frag`, `.import/shaders/${UUID}/shader.vulkan.frag.spv`,
  ]);
  const direct = cookShader(fs.readFileSync(path.join(dir, 'assets/fx/solid.n2shader'), 'utf8'), { path: 'assets/fx/solid.n2shader', targets: ['vulkan', 'gl'], tools });
  assert.ok(direct.ok);
  const byName = new Map(result.entries.map((entry) => [entry.name.split('/').pop(), entry.data]));
  assert.ok(byName.get('manifest.json').equals(manifestBytes(direct.manifest)));
  assert.ok(byName.get('shader.vulkan.frag.spv').equals(direct.outputs.vulkan.data));
  assert.ok(byName.get('shader.gl.frag').equals(direct.outputs.gl.data));
  assert.deepEqual(Object.keys(JSON.parse(byName.get('manifest.json')).payloads), ['vulkan', 'gl']);
  // deterministic: the same project, the same bytes
  const again = cookProjectShaders({ projectDir: dir, targets: ['vulkan', 'gl'], tools });
  assert.deepEqual(again.entries.map((e) => [e.name, e.data.toString('hex')]), result.entries.map((e) => [e.name, e.data.toString('hex')]));
});

test('strip: no GL text, the .n2shader keeps its header only', { skip: skipCore }, () => {
  const dir = makeProject();
  const result = cookProjectShaders({ projectDir: dir, targets: ['vulkan', 'gl'], strip: true, tools });
  assert.deepEqual(names(result.entries), [`.import/shaders/${UUID}/manifest.json`, `.import/shaders/${UUID}/shader.vulkan.frag.spv`]);
  assert.ok(!('gl' in JSON.parse(result.entries[0].data).payloads));
  assert.ok(result.replaced.get('assets/fx/solid.n2shader').toString().endsWith('#stage fragment\n'));
});

test('a web pack carries WGSL only', { skip: skipTint }, () => {
  const result = cookProjectShaders({ projectDir: makeProject(), targets: ['webgpu'], tools });
  assert.deepEqual(names(result.entries), [`.import/shaders/${UUID}/manifest.json`, `.import/shaders/${UUID}/shader.webgpu.wgsl`]);
});

/** The entries of a zip written by writeZip, name -> bytes (stored or deflated) */
function readZip(file) {
  const buffer = fs.readFileSync(file);
  const out = new Map();
  let offset = buffer.length - 22;
  const count = buffer.readUInt16LE(offset + 10);
  let at = buffer.readUInt32LE(offset + 16);
  for (let i = 0; i < count; i++) {
    const method = buffer.readUInt16LE(at + 10);
    const size = buffer.readUInt32LE(at + 20);
    const nameLength = buffer.readUInt16LE(at + 28);
    const local = buffer.readUInt32LE(at + 42);
    const name = buffer.toString('utf8', at + 46, at + 46 + nameLength);
    const start = local + 30 + buffer.readUInt16LE(local + 26) + buffer.readUInt16LE(local + 28);
    const body = buffer.subarray(start, start + size);
    out.set(name, method === 8 ? zlib.inflateRawSync(body) : Buffer.from(body));
    at += 46 + nameLength;
  }
  return out;
}

test('a pack holds this target\'s cooked shaders in place of the editor\'s, and a stripped source', { skip: skipCore }, () => {
  const dir = makeProject({ extra: { [`.import/shaders/${UUID}/shader.webgpu.wgsl`]: 'editor leftover', '.import/shaders/old-uuid/manifest.json': '{}' } });
  const out = path.join(tmp('n2build-pack-'), 'game.n2pack');
  const manifest = cookPack({ projectDir: dir, outFile: out, format: 'stored', target: 'windows', game: 'Game', skipModuleCheck: true, shaderTargets: ['vulkan', 'gl'], shaderTools: tools });
  const files = readZip(out);
  const shaderFiles = [...files.keys()].filter((name) => name.startsWith('.import/shaders/'));
  assert.deepEqual(shaderFiles, [`.import/shaders/${UUID}/manifest.json`, `.import/shaders/${UUID}/shader.gl.frag`, `.import/shaders/${UUID}/shader.vulkan.frag.spv`]);
  assert.ok(files.get('assets/fx/solid.n2shader').toString().includes('void main'));
  assert.deepEqual([...files.keys()], [...files.keys()].sort());
  assert.ok(manifest.files.some((f) => f.path.endsWith('shader.vulkan.frag.spv')));
  assert.ok(files.has(MANIFEST_NAME));

  const stripped = path.join(tmp('n2build-pack-'), 'game.n2pack');
  cookPack({ projectDir: dir, outFile: stripped, format: 'stored', target: 'windows', game: 'Game', skipModuleCheck: true, shaderTargets: ['vulkan', 'gl'], stripShaderSources: true, shaderTools: tools });
  const strippedFiles = readZip(stripped);
  assert.ok(!strippedFiles.get('assets/fx/solid.n2shader').toString().includes('void main'));
  assert.ok(![...strippedFiles.keys()].some((name) => name.endsWith('.gl.frag')));
  // the same project makes the same pack
  const again = path.join(tmp('n2build-pack-'), 'game.n2pack');
  cookPack({ projectDir: dir, outFile: again, format: 'stored', target: 'windows', game: 'Game', skipModuleCheck: true, shaderTargets: ['vulkan', 'gl'], shaderTools: tools });
  assert.ok(fs.readFileSync(again).equals(fs.readFileSync(out)));
});

// ---- Tint: the pinned download ----

function tarEntry(name, data) {
  const header = Buffer.alloc(512);
  header.write(name, 0, 'latin1');
  header.write('0000644\0', 100);
  header.write(`${data.length.toString(8).padStart(11, '0')}\0`, 124);
  header.write('0', 156);
  header.write('ustar\0', 257);
  const padding = Buffer.alloc((512 - (data.length % 512)) % 512);
  return Buffer.concat([header, data, padding]);
}

function tarGz(entries) {
  return zlib.gzipSync(Buffer.concat([...entries.map(([name, data]) => tarEntry(name, data)), Buffer.alloc(1024)]));
}

async function* chunked(buffer, size = 97) {
  for (let i = 0; i < buffer.length; i += size) yield buffer.subarray(i, i + size);
}

test('extractTarGzEntry takes one entry out of a gzip stream, whatever the chunking', async () => {
  const wanted = crypto.randomBytes(3000);
  const archive = tarGz([['Dawn-x/readme', Buffer.from('hello')], ['Dawn-x/lib/big.bin', crypto.randomBytes(1500)], ['Dawn-x/bin/tint', wanted], ['Dawn-x/after', Buffer.from('after')]]);
  assert.ok((await extractTarGzEntry(chunked(archive), 'bin/tint')).equals(wanted));
  assert.ok((await extractTarGzEntry(chunked(archive, 5000), 'bin/tint')).equals(wanted));
  assert.equal(await extractTarGzEntry(chunked(archive), 'bin/nothing'), null);
});

test('installTint keeps the binary only when its hash is the pinned one', async () => {
  const platform = 'linux';
  const arch = 'x64';
  const binary = Buffer.from('#!tint\n'.repeat(100));
  const archive = tarGz([[`Dawn-${DAWN_COMMIT}-ubuntu-latest-Release/bin/tint`, binary]]);
  const home = tmp('n2build-home-');
  const env = { HOME: home };
  const requested = [];
  const fetchImpl = async (url) => {
    requested.push(url);
    return new Response(archive, { status: 200 });
  };
  const pins = { 'linux-x64': { ...TINT_PINS['linux-x64'], sha256: sha256Of(binary) } };
  const file = await installTint({ env, platform, arch, fetchImpl, pins });
  assert.deepEqual(requested, [archiveUrl(TINT_PINS['linux-x64'])]);
  assert.ok(fs.readFileSync(file).equals(binary));
  assert.ok(file.startsWith(toolsFolder(env, platform)));

  const wrong = { 'linux-x64': { ...TINT_PINS['linux-x64'], sha256: '0'.repeat(64) } };
  const home2 = tmp('n2build-home-');
  await assert.rejects(installTint({ env: { HOME: home2 }, platform, arch, fetchImpl, pins: wrong }), /not the pinned/);
  assert.deepEqual(fs.existsSync(path.join(home2, '.local')), false, 'nothing is kept when the hash is wrong');
  await assert.rejects(installTint({ env, platform, arch, fetchImpl: async () => new Response('', { status: 404 }), pins }), /HTTP 404/);
  await assert.rejects(installTint({ env, platform: 'plan9', arch, fetchImpl, pins }), /no pinned Tint for plan9-x64/);
});

test('cachedTint finds the kept binary only while it is the pinned one', () => {
  assert.equal(cachedTint({ env: { HOME: tmp('n2build-home-') }, platform: 'linux', arch: 'x64' }), null);
  assert.equal(tintKey('win32', 'x64'), 'win32-x64');
  for (const key of Object.keys(TINT_PINS)) assert.match(TINT_PINS[key].sha256, /^[0-9a-f]{64}$/, key);
});

test('writeZip is still deterministic with shader entries (a smoke check of the entry order)', () => {
  const a = writeZip([{ name: 'a', data: Buffer.from('1') }, { name: 'b', data: Buffer.from('2') }]);
  assert.ok(a.equals(writeZip([{ name: 'a', data: Buffer.from('1') }, { name: 'b', data: Buffer.from('2') }])));
});
