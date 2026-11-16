// Tests for the strict floating-point answer of scripts/project-modules.mjs. Run: node --test scripts/project-strict-fp.test.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

import { networkedPhysicsObjectsIn, resolveStrictFp, scanProject, strictFpWarning } from './project-modules.mjs';

function makeProject(project, files = {}) {
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'n2-strictfp-'));
  fs.writeFileSync(path.join(folder, 'project.n2proj'), JSON.stringify(project));
  for (const [relative, content] of Object.entries(files)) {
    const full = path.join(folder, relative);
    fs.mkdirSync(path.dirname(full), { recursive: true });
    fs.writeFileSync(full, JSON.stringify(content));
  }
  return folder;
}

const sceneWith = (...types) => ({
  rootGameObjects: [{ name: 'A', components: types.map((type) => ({ type, data: {} })), children: [] }],
});

test('networkedPhysicsObjectsIn counts objects holding a network and a physics component together', () => {
  const doc = {
    rootGameObjects: [
      { name: 'Both', components: [{ type: 'NetworkObject' }, { type: 'Rigidbody' }], children: [] },
      { name: 'Both2D', components: [{ type: 'NetworkTransform' }, { type: 'BoxCollider2D' }], children: [] },
      { name: 'NetOnly', components: [{ type: 'NetworkObject' }], children: [{ name: 'Child', components: [{ type: 'Rigidbody' }], children: [] }] },
    ],
  };
  assert.equal(networkedPhysicsObjectsIn(doc), 2);
  assert.equal(networkedPhysicsObjectsIn({}), 0);
});

test('resolveStrictFp: the project key wins, else on for a networked game, else the build default', () => {
  const net = makeProject({ name: 'G' }, { 'assets/a.scene': sceneWith('NetworkObject') });
  const quiet = makeProject({ name: 'G' }, { 'assets/a.scene': sceneWith('Rigidbody') });
  assert.equal(resolveStrictFp(scanProject(net)), true);
  assert.equal(resolveStrictFp(scanProject(quiet)), null);
  const off = makeProject({ name: 'G', build: { strictFp: false } }, { 'assets/a.scene': sceneWith('NetworkObject') });
  assert.equal(resolveStrictFp(scanProject(off)), false);
  const on = makeProject({ name: 'G', build: { strictFp: true } });
  assert.equal(resolveStrictFp(scanProject(on)), true);
  const preset = makeProject({ name: 'G', settings: { network: { preset: 'authoritative' } } });
  assert.equal(resolveStrictFp(scanProject(preset)), true);
  assert.throws(() => scanProject(makeProject({ name: 'G', build: { strictFp: 'yes' } })), /build\.strictFp must be true or false/);
});

test('strictFpWarning fires only for strictFp false with networked physics objects', () => {
  const scene = { 'assets/a.scene': sceneWith('NetworkObject', 'Rigidbody') };
  const warned = strictFpWarning(scanProject(makeProject({ name: 'G', build: { strictFp: false } }, scene)));
  assert.match(warned, /build\.strictFp is false, but 1 object .*assets\/a\.scene/);
  assert.equal(strictFpWarning(scanProject(makeProject({ name: 'G' }, scene))), null, 'unset: the build turns it on');
  assert.equal(strictFpWarning(scanProject(makeProject({ name: 'G', build: { strictFp: true } }, scene))), null);
  const apart = { 'assets/a.scene': sceneWith('NetworkObject'), 'assets/b.scene': sceneWith('Rigidbody') };
  assert.equal(strictFpWarning(scanProject(makeProject({ name: 'G', build: { strictFp: false } }, apart))), null);
});
