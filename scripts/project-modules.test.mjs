// Tests for scripts/project-modules.mjs. Run: node --test scripts/project-modules.test.mjs
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  componentTypesIn,
  findConflicts,
  readModuleList,
  scanLua,
  scanProject,
  stripLuaComments,
  suggestList,
} from './project-modules.mjs';

const script = path.join(path.dirname(fileURLToPath(import.meta.url)), 'project-modules.mjs');

function makeProject(project, files = {}) {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'n2-modules-'));
  fs.writeFileSync(path.join(folder, 'project.n2proj'), JSON.stringify(project));
  for (const [relative, content] of Object.entries(files)) {
    const full = path.join(folder, relative);
    fs.mkdirSync(path.dirname(full), { recursive: true });
    fs.writeFileSync(full, typeof content === 'string' ? content : JSON.stringify(content));
  }
  return folder;
}

function run(...args) {
  return spawnSync(process.execPath, [script, ...args], { encoding: 'utf8' });
}

const sceneWith = (...types) => ({
  rootGameObjects: [{ name: 'A', components: types.map((type) => ({ type, data: {} })), children: [] }],
});

test('Lua comments are removed and strings are kept', () => {
  assert.equal(stripLuaComments('a = 1 -- gone\nb = 2'), 'a = 1 \nb = 2');
  assert.equal(stripLuaComments('--[[ gone\nstill gone ]]x'), 'x');
  assert.equal(stripLuaComments('--[==[ gone ]] still ]==]x'), 'x');
  assert.equal(stripLuaComments('s = "a -- not a comment"'), 's = "a -- not a comment"');
  assert.equal(stripLuaComments("s = 'it\\'s -- kept'"), "s = 'it\\'s -- kept'");
  assert.equal(stripLuaComments('s = [[ -- kept ]]'), 's = [[ -- kept ]]');
});

test('a Lua use is found by global, handler, enum and component name', () => {
  const modules = (source) => scanLua(source).uses.map((use) => use.module);
  assert.deepEqual(modules('Physics.Raycast(a, b)'), ['physics3d']);
  assert.deepEqual(modules('function X:OnCollisionEnter(c) end'), ['physics3d']);
  assert.deepEqual(modules('function X:OnTriggerStay(c) end'), ['physics3d']);
  assert.deepEqual(modules('rb:SetBodyType(BodyType.Dynamic)'), ['physics3d']);
  assert.deepEqual(modules('go:AddComponent("Rigidbody")'), ['physics3d']);
  assert.deepEqual(modules("go:GetComponent('BoxCollider')"), ['physics3d']);
  assert.deepEqual(modules('Physics2D.Raycast(a, b)'), ['physics2d']);
  assert.deepEqual(modules('function X:OnCollisionEnter2D(c) end'), ['physics2d']);
  assert.deepEqual(modules('rb:SetBodyType(BodyType2D.Dynamic)'), ['physics2d']);
  assert.deepEqual(modules('go:AddComponent("CircleCollider2D")'), ['physics2d']);
});

test('the 2D and 3D names do not match each other', () => {
  assert.deepEqual(scanLua('Physics2D.Raycast(a, b)').uses.filter((use) => use.module === 'physics3d'), []);
  assert.deepEqual(scanLua('go:AddComponent("Rigidbody2D")').uses.filter((use) => use.module === 'physics3d'), []);
  assert.deepEqual(scanLua('function X:OnCollisionEnter(c) end').uses.filter((use) => use.module === 'physics2d'), []);
});

test('a commented-out use does not count, and an unrelated name does not match', () => {
  assert.deepEqual(scanLua('-- go:AddComponent("Rigidbody")\n--[[ Physics.Raycast() ]]').uses, []);
  assert.deepEqual(scanLua('local MyPhysics = {}; MyPhysics.Raycast = 1; local Rigidbody = 2').uses, []);
});

test('a use reports its line', () => {
  const [use] = scanLua('\n\ngo:AddComponent("Rigidbody")').uses;
  assert.equal(use.line, 3);
});

test('a component call with a computed name is reported, and a literal one is not', () => {
  assert.equal(scanLua('go:AddComponent("Light")').unresolved.length, 0);
  assert.equal(scanLua("go:GetComponent('Light')").unresolved.length, 0);
  const { unresolved } = scanLua('\ngo:AddComponent(kinds[i])');
  assert.equal(unresolved.length, 1);
  assert.equal(unresolved[0].line, 2);
  assert.equal(scanLua('go:AddComponent(prefix .. "2D")').unresolved.length, 1);
});

test('component types are found at any depth', () => {
  const types = componentTypesIn({
    rootGameObjects: [{ components: [{ type: 'Light' }], children: [{ components: [{ type: 'Rigidbody2D' }], children: [] }] }],
  });
  assert.deepEqual([...types].sort(), ['Light', 'Rigidbody2D']);
});

test('the list is read and checked', () => {
  assert.equal(readModuleList({}), null);
  assert.deepEqual(readModuleList({ build: { modules: { physics3d: 'none' } } }), { physics3d: 'none' });
  assert.throws(() => readModuleList({ build: { modules: { physics3d: 'havok' } } }), /physics3d must be one of/);
  assert.throws(() => readModuleList({ build: { modules: { physics2d: 'no' } } }), /physics2d must be true or false/);
  assert.throws(() => readModuleList({ build: { modules: { audio: false } } }), /unknown module "audio"/);
  assert.throws(() => readModuleList({ build: { modules: [] } }), /must be an object/);
});

test('with no list nothing can conflict, whatever the project uses', () => {
  const folder = makeProject({ name: 'p' }, { 'assets/scene.lua': 'Physics.Raycast(a, b)' });
  const scan = scanProject(folder);
  assert.equal(scan.list, null);
  assert.deepEqual(findConflicts(scan), []);
});

test('a stripped module that a script uses is a conflict naming the file and line', () => {
  const folder = makeProject(
    { name: 'p', build: { modules: { physics3d: 'none' } } },
    { 'assets/scene.lua': '\ngo:AddComponent("Rigidbody")' },
  );
  const [conflict] = findConflicts(scanProject(folder));
  assert.match(conflict, /assets\/scene\.lua:2 uses Rigidbody/);
  assert.match(conflict, /leaves physics3d out/);
});

test('a stripped module that a scene or prefab uses is a conflict', () => {
  const scene = makeProject({ name: 'p', build: { modules: { physics2d: false } } }, { 'scenes/level.scene': sceneWith('Light', 'BoxCollider2D') });
  assert.match(findConflicts(scanProject(scene))[0], /scenes\/level\.scene uses BoxCollider2D/);
  const prefab = makeProject({ name: 'p', build: { modules: { physics3d: 'none' } } }, { 'assets/crate.prefab': sceneWith('Rigidbody') });
  assert.match(findConflicts(scanProject(prefab))[0], /assets\/crate\.prefab uses Rigidbody/);
});

test('a backend that is kept is not a conflict', () => {
  const folder = makeProject({ name: 'p', build: { modules: { physics3d: 'jolt', physics2d: true } } }, {
    'assets/scene.lua': 'go:AddComponent("Rigidbody"); Physics2D.Raycast(a, b)',
  });
  assert.deepEqual(findConflicts(scanProject(folder)), []);
});

test('settings that need a backend the list leaves out are a conflict', () => {
  const needs = (settings, physics3d) => findConflicts(scanProject(makeProject({ name: 'p', settings, build: { modules: { physics3d } } })));
  assert.match(needs({ network: { preset: 'deterministic' } }, 'physx')[0], /need the Jolt backend/);
  assert.match(needs({ physics: { backend: 'jolt' } }, 'none')[0], /need the Jolt backend/);
  assert.match(needs({ physics: { backend: 'physx' } }, 'jolt')[0], /ask for physics\.backend "physx"/);
  assert.deepEqual(needs({ network: { preset: 'deterministic' } }, 'both'), []);
  assert.deepEqual(needs({ physics: { backend: 'physx' } }, 'physx'), []);
});

test('generated and hidden folders are not scanned, and a broken scene is reported, not fatal', () => {
  const folder = makeProject({ name: 'p', build: { modules: { physics3d: 'none' } } }, {
    'assets/.import/cache.lua': 'Physics.Raycast(a, b)',
    'assets/broken.scene': '{ not json',
  });
  const scan = scanProject(folder);
  assert.deepEqual(findConflicts(scan), []);
  assert.deepEqual(scan.unreadable, ['assets/broken.scene']);
});

test('the suggestion is the smallest list the scan can justify', () => {
  const suggest = (project, files) => suggestList(scanProject(makeProject({ name: 'p', ...project }, files)));
  assert.deepEqual(suggest({}, {}), { physics3d: 'none', physics2d: false });
  assert.deepEqual(suggest({}, { 'assets/a.lua': 'Physics2D.Raycast(a, b)' }), { physics3d: 'none', physics2d: true });
  assert.deepEqual(suggest({}, { 'assets/a.lua': 'Physics.Raycast(a, b)' }), { physics3d: 'physx', physics2d: false });
  assert.deepEqual(suggest({ settings: { network: { preset: 'deterministic' } } }, {}), { physics3d: 'jolt', physics2d: false });
});

test('the command line exits 0 when clean, 1 on a conflict and 2 on bad use', () => {
  const clean = makeProject({ name: 'p', build: { modules: { physics3d: 'none', physics2d: false } } }, { 'assets/scene.lua': 'print("hi")' });
  assert.equal(run(clean).status, 0);

  const conflicted = makeProject({ name: 'p', build: { modules: { physics3d: 'none' } } }, { 'assets/scene.lua': 'Physics.Raycast(a, b)' });
  const result = run(conflicted);
  assert.equal(result.status, 1);
  assert.match(result.stderr, /CONFLICT: assets\/scene\.lua:1 uses Physics\.R/);

  assert.equal(run().status, 2);
  assert.equal(run(clean, '--nonsense').status, 2);
  assert.equal(run(path.join(clean, 'missing')).status, 2);
  assert.equal(run(makeProject({ name: 'p', build: { modules: { physics3d: 'havok' } } })).status, 2);
});

test('--suggest prints a block that can be pasted, and --json the whole report', () => {
  const folder = makeProject({ name: 'p' }, { 'assets/a.lua': 'Physics2D.Raycast(a, b)\ngo:AddComponent(kind)' });
  const suggested = run(folder, '--suggest');
  assert.equal(suggested.status, 0);
  assert.deepEqual(JSON.parse(suggested.stdout), { build: { modules: { physics3d: 'none', physics2d: true } } });
  assert.match(suggested.stderr, /computed name/);

  const report = JSON.parse(run(folder, '--json').stdout);
  assert.equal(report.unresolved.length, 1);
  assert.deepEqual(report.conflicts, []);
});
