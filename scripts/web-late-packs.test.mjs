// node --test scripts/web-late-packs.test.mjs: the late packs of a web package (#425, docs/web-build.html#late-packs):
// the writer (scripts/web-late-packs.mjs) and the loader's fetch of them after the start (web/n2-loader.js fetchLatePacks)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { createRequire } from 'node:module';
import { parseFolders, folderProblem, writeLatePacks } from './web-late-packs.mjs';

const require = createRequire(import.meta.url);
const N2Loader = require('../web/n2-loader.js');

function tempProject() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'n2-late-'));
  fs.mkdirSync(path.join(root, 'assets', 'levels', 'two'), { recursive: true });
  fs.writeFileSync(path.join(root, 'assets', 'levels', 'One.scene'), '{"name":"One"}');
  fs.writeFileSync(path.join(root, 'assets', 'levels', 'two', 'Two.scene'), '{"name":"Two"}');
  fs.mkdirSync(path.join(root, 'out'));
  return root;
}

test('folders are parsed and checked: inside assets/, existing, no overlap', () => {
  assert.deepEqual(parseFolders('assets/a, assets/b/ ,,'), ['assets/a', 'assets/b']);
  const root = tempProject();
  assert.equal(folderProblem(root, 'assets/levels'), null);
  assert.match(folderProblem(root, 'levels'), /inside assets/);
  assert.match(folderProblem(root, 'assets/../x'), /inside assets/);
  assert.match(folderProblem(root, 'assets/missing'), /no such folder/);
  assert.throws(() => writeLatePacks(root, path.join(root, 'out'), ['assets/levels', 'assets/levels/two']), /overlap/);
});

test('each folder becomes a stored zip of its files, relative to the folder, the same bytes every time', () => {
  const root = tempProject();
  const out = path.join(root, 'out');
  const packs = writeLatePacks(root, out, ['assets/levels']);
  assert.deepEqual(packs.map((p) => [p.file, p.folder, p.files]), [['late-0.n2pack', 'assets/levels', 2]]);
  const zip = fs.readFileSync(path.join(out, 'late-0.n2pack'));
  assert.equal(zip.readUInt32LE(0), 0x04034b50, 'a zip');
  assert.ok(zip.includes(Buffer.from('One.scene')) && zip.includes(Buffer.from('two/Two.scene')), 'names relative to the folder');
  assert.ok(!zip.includes(Buffer.from('assets/levels/')), 'not the project path');
  const again = writeLatePacks(root, out, ['assets/levels']);
  assert.equal(again[0].bytes, zip.length);
  assert.ok(fs.readFileSync(path.join(out, 'late-0.n2pack')).equals(zip), 'reproducible');
});

test('the loader fetches every late pack after the start, writes it and has the player mount it, in order', async () => {
  const packs = { 'late-0.n2pack': Buffer.from('first pack'), 'late-1.n2pack': Buffer.from('second pack!') };
  const files = {};
  for (const [name, data] of Object.entries(packs)) {
    files[name] = { raw: data.length, sha256: crypto.createHash('sha256').update(data).digest('hex') };
  }
  const manifest = { format: 1, files, latePacks: [{ file: 'late-0.n2pack', folder: 'assets/a' }, { file: 'late-1.n2pack', folder: 'assets/b' }] };
  const requested = [];
  const fetchFn = async (url, init) => {
    requested.push(url);
    if (url === 'manifest.json') return new Response(JSON.stringify(manifest), { headers: { 'content-type': 'application/json' } });
    const name = url.split('?')[0];
    if (packs[name]) return new Response(packs[name]);
    return new Response('nope', { status: 404 });
  };
  const events = [];
  const loader = N2Loader.create({ fetch: fetchFn, onEvent: (name, detail) => events.push([name, detail]) });
  // Only the manifest: run() would go on to the pack and the wasm, which this test does not have
  loader.run(() => new Promise(() => {}), {}).catch(() => {});
  await new Promise((resolve) => setTimeout(resolve, 20));
  const written = [];
  const mounts = [];
  const engine = {
    FS: { writeFile: (file, bytes) => written.push([file, Buffer.from(bytes).toString()]) },
    ccall: (name, type, types, args) => { mounts.push([name, ...args]); return args[1] === 'assets/b' ? 0 : 1; },
  };
  const mounted = await loader.fetchLatePacks(engine);
  assert.equal(mounted, 1, 'the player refused the second (0): it stays pending');
  assert.deepEqual(written, [['/late-0.n2pack', 'first pack'], ['/late-1.n2pack', 'second pack!']]);
  assert.deepEqual(mounts, [['n2_mount_late_pack', '/late-0.n2pack', 'assets/a'], ['n2_mount_late_pack', '/late-1.n2pack', 'assets/b']]);
  assert.ok(requested.includes(`late-0.n2pack?v=${files['late-0.n2pack'].sha256.slice(0, 12)}`), 'fetched by its hash');
  assert.deepEqual(events.filter(([n]) => n.startsWith('late-pack')).map(([n, d]) => [n, d.file]),
    [['late-pack-ready', 'late-0.n2pack'], ['late-pack-failed', 'late-1.n2pack']]);
});

test('a pack of the wrong size is not mounted, and a package without late packs fetches nothing', async () => {
  const data = Buffer.from('short');
  const manifest = { format: 1, files: { 'late-0.n2pack': { raw: 99, sha256: 'a'.repeat(64) } }, latePacks: [{ file: 'late-0.n2pack', folder: 'assets/a' }] };
  const fetchFn = async (url) => (url === 'manifest.json' ? new Response(JSON.stringify(manifest)) : new Response(data));
  const events = [];
  const loader = N2Loader.create({ fetch: fetchFn, onEvent: (name, detail) => events.push([name, detail]) });
  loader.run(() => new Promise(() => {}), {}).catch(() => {});
  await new Promise((resolve) => setTimeout(resolve, 20));
  let calls = 0;
  const mounted = await loader.fetchLatePacks({ FS: { writeFile: () => calls++ }, ccall: () => { calls++; return 1; } });
  assert.equal(mounted, 0);
  assert.equal(calls, 0, 'nothing written or mounted');
  assert.match(events.find(([n]) => n === 'late-pack-failed')[1].reason, /99/);

  const none = N2Loader.create({ fetch: async () => new Response(JSON.stringify({ format: 1, files: {} })) });
  none.run(() => new Promise(() => {}), {}).catch(() => {});
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(await none.fetchLatePacks({}), 0);
});
