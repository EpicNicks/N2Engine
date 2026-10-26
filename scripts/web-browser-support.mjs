// Pure assertions shared by the browser gate and its negative-control unit tests.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

export function browserOptions(args) {
  const known = new Set(['--unsafe-webgpu', '--swiftshader', '--headed']);
  for (const arg of args.filter(value => value.startsWith('--'))) {
    assert(known.has(arg), `Unknown browser smoke option: ${arg}`);
  }
  const positional = args.filter(value => !value.startsWith('--'));
  assert(positional.length <= 2, 'Expected web-package and optional Playwright prefix');
  return {
    root: path.resolve(positional[0] || 'build-web/web-package'),
    tools: path.resolve(positional[1] || 'scripts/browser-tools'),
    swiftshader: args.includes('--swiftshader'),
    headed: args.includes('--headed'),
    unsafe: args.includes('--unsafe-webgpu') || args.includes('--swiftshader'),
  };
}

export function browserLaunchArgs(options) {
  const args = ['--host-resolver-rules=MAP n2-insecure.test 127.0.0.1', '--no-proxy-server'];
  if (options.unsafe) args.push('--enable-unsafe-webgpu');
  // Chromium's Dawn selector forces the fallback WebGPU adapter, independently of ANGLE's compositor.
  if (options.swiftshader) args.push('--use-webgpu-adapter=swiftshader', '--use-angle=vulkan',
    '--enable-features=Vulkan', '--use-vulkan=swiftshader', '--disable-vulkan-surface');
  return args;
}

export function requireBrowserInputs(root, readbackRoot, requiredReadback) {
  for (const name of ['index.html', 'index.js', 'index.wasm', 'index.data', 'n2-loader.js', 'manifest.json']) {
    assert(fs.statSync(path.join(root, name)).isFile(), `Missing browser package input: ${name}`);
  }
  assert(!requiredReadback || readbackRoot, 'SwiftShader CI requires N2_READBACK_DIR: deterministic readback tests must not be skipped');
  if (readbackRoot) {
    for (const name of ['n2-webgpu-readback.js', 'n2-webgpu-readback.wasm']) {
      assert(fs.statSync(path.join(readbackRoot, name)).isFile(), `Missing readback input: ${name}`);
    }
  }
}

export function requireSwiftShader(info) {
  assert(info && /swiftshader/i.test([info.vendor, info.architecture, info.device, info.description].join(' ')),
    `Expected SwiftShader WebGPU adapter, received ${JSON.stringify(info)}`);
}

// Inspect the COMPOSITED canvas screenshot, not a mock, frame counter or GPU canvas toDataURL (which may be cleared).
// The demo animates by wall time, so do not compare it with a timing-dependent screenshot hash. Exact deterministic
// GPU pixel goldens run separately through WebGpuReadback.cpp's fixed-colour scene and asynchronous map callbacks.
export function requireVisibleCanvas({ width, height, data }) {
  assert(Number.isInteger(width) && width > 0 && Number.isInteger(height) && height > 0, 'Invalid canvas screenshot size');
  assert.equal(data.length, width * height * 4, 'Canvas screenshot must contain RGBA pixels');
  const colours = new Uint32Array(4096); // 16 bins per channel, ignoring insignificant colour noise.
  let dominant = 0;
  for (let i = 0; i < data.length; i += 4) {
    const bucket = ((data[i] >> 4) << 8) | ((data[i + 1] >> 4) << 4) | (data[i + 2] >> 4);
    dominant = Math.max(dominant, ++colours[bucket]);
  }
  const varied = width * height - dominant;
  assert(varied >= Math.max(32, width * height * 0.001), `Canvas is blank or nearly uniform (${varied} varied pixels)`);
  return { width, height, variedPixels: varied };
}
