import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { browserOptions, browserLaunchArgs, requireBrowserInputs, requireSwiftShader, requireVisibleCanvas } from './web-browser-support.mjs';

test('SwiftShader is explicit, forces Dawn fallback, and unknown flags are refused', () => {
  assert(!browserLaunchArgs(browserOptions([])).includes('--enable-unsafe-webgpu'));
  const options = browserOptions(['--swiftshader', 'package', 'tools']);
  assert(options.unsafe);
  assert.equal(options.root, path.resolve('package'));
  assert(browserLaunchArgs(options).includes('--use-webgpu-adapter=swiftshader'));
  assert.throws(() => browserOptions(['--swiftshder']));
});

test('hardware and missing adapters cannot silently satisfy the SwiftShader CI gate', () => {
  assert.doesNotThrow(() => requireSwiftShader({ description: 'SwiftShader Device (Subzero)' }));
  assert.throws(() => requireSwiftShader({ vendor: 'Intel', description: 'hardware' }));
  assert.throws(() => requireSwiftShader(null));
});

test('blank, uniform and almost-empty composited screenshots fail', () => {
  const image = { width: 64, height: 64, data: new Uint8Array(64 * 64 * 4) };
  assert.throws(() => requireVisibleCanvas(image));
  image.data.fill(180);
  assert.throws(() => requireVisibleCanvas(image));
  image.data[0] = 0;
  assert.throws(() => requireVisibleCanvas(image), 'One noisy corner is still an almost-empty frame');
  image.data[0] = 180;
  image.data[4] = 0;
  assert.throws(() => requireVisibleCanvas(image));
  for (let i = 4; i < 404; i += 4) image.data[i] = 0;
  assert.equal(requireVisibleCanvas(image).variedPixels, 100);
  assert.throws(() => requireVisibleCanvas({ ...image, data: new Uint8Array(3) }));
});

test('missing deterministic input artifacts fail instead of skipping the browser golden', () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'n2-browser-inputs-'));
  try {
    for (const name of ['index.html', 'index.js', 'index.wasm', 'index.data', 'n2-loader.js', 'manifest.json']) fs.writeFileSync(path.join(root, name), 'test');
    assert.doesNotThrow(() => requireBrowserInputs(root, null, false));
    assert.throws(() => requireBrowserInputs(root, null, true));
    assert.throws(() => requireBrowserInputs(root, root, true));
    for (const name of ['n2-webgpu-readback.js', 'n2-webgpu-readback.wasm']) fs.writeFileSync(path.join(root, name), 'test');
    assert.doesNotThrow(() => requireBrowserInputs(root, root, true));
    fs.unlinkSync(path.join(root, 'index.wasm'));
    assert.throws(() => requireBrowserInputs(root, root, true));
  } finally {
    assert.equal(path.dirname(root), path.resolve(os.tmpdir()));
    assert(path.basename(root).startsWith('n2-browser-inputs-'));
    fs.rmSync(root, { recursive: true, force: true });
  }
});
