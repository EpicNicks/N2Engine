// Selection and recovery use the same code as the packaged page. No GPU needed.
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';
import { test } from 'node:test';
const { createGpuProbe, createGpuSession, createGpuPresenter, presenterWgsl } = createRequire(import.meta.url)('../web/n2-loader.js');
const deferred = () => { let resolve; const promise = new Promise(r => { resolve = r; }); return { promise, resolve }; };
function device() {
  const lost = deferred();
  const result = { lost: lost.promise, lose: lost.resolve, destroyed: false,
    destroy() { this.destroyed = true; }, addEventListener(name, fn) { this[name] = fn; } };
  return result;
}
const tick = () => new Promise(r => setImmediate(r));
const probe = (gpu, extra = {}) => createGpuProbe({ secure: true, search: '', navigator: { gpu }, ...extra });

test('software override and insecure context never request an adapter', async () => {
  let calls = 0;
  const gpu = { requestAdapter() { calls++; throw Error('unreachable'); } };
  assert.equal((await probe(gpu, { search: '?renderer=software' }).acquire()).renderer, 'software');
  assert.equal((await probe(gpu, { secure: false }).acquire()).renderer, 'software');
  assert.equal((await probe(gpu, { search: '?renderer=bogus' }).acquire()).renderer, 'software');
  assert.equal(calls, 0);
});
test('missing GPU, null adapter and request failures select software', async () => {
  for (const gpu of [undefined, { requestAdapter: async () => null },
    { requestAdapter: async () => { throw Error('adapter refused'); } },
    { requestAdapter: async () => ({ requestDevice: async () => { throw Error('device refused'); } }) }]) {
    assert.equal((await probe(gpu).acquire()).renderer, 'software');
  }
});
test('automatic and explicit WebGPU return the acquired device', async () => {
  const acquired = device();
  const gpu = { requestAdapter: async options => {
    assert.equal(options.powerPreference, 'high-performance');
    return { requestDevice: async () => acquired };
  } };
  for (const search of ['', '?renderer=webgpu']) {
    const result = await probe(gpu, { search }).acquire();
    assert.equal(result.renderer, 'webgpu');
    assert.equal(result.device, acquired);
  }
});
test('timeout bounds adapter and device requests and destroys late devices', async () => {
  const adapter = deferred();
  const request = probe({ requestAdapter: () => adapter.promise }, { timeoutMs: 5 }).acquire();
  assert.match((await request).reason, /timed out/);
  let requested = false;
  adapter.resolve({ requestDevice() { requested = true; } });
  await tick();
  assert.equal(requested, false);
  const pending = deferred();
  const acquired = device();
  const result = await probe({ requestAdapter: async () => ({ requestDevice: () => pending.promise }) }, { timeoutMs: 5 }).acquire();
  assert.equal(result.renderer, 'software');
  pending.resolve(acquired);
  await tick();
  assert.equal(acquired.destroyed, true);
});
test('device loss pauses, reacquires once and signals a resource rebuild once', async () => {
  const first = device(), second = device();
  const pending = deferred();
  let attempts = 0;
  const failures = [], drawn = [];
  const session = createGpuSession({ device: first, probe: { acquire() { attempts++; return pending.promise; } },
    createPresenter: d => texture => drawn.push([d, texture]), onFailure: reason => failures.push(reason) });
  session.present('first frame');
  first.lose({ message: 'reset' });
  await tick();
  assert.equal(session.status(), 0);
  session.present('skipped');
  pending.resolve({ device: second });
  await tick();
  assert.equal(session.device, second);
  assert.equal(session.status(), 2);
  assert.equal(session.status(), 1);
  session.present('restored');
  second.lose({ message: 'reset again' });
  await tick();
  assert.equal(session.status(), 0);
  assert.equal(attempts, 1);
  assert.deepEqual(failures, ['reset again']);
  assert.deepEqual(drawn, [[first, 'first frame'], [second, 'restored']]);
});
test('failed reacquisition and validation storms stop with a fallback action', async () => {
  const first = device();
  const failures = [];
  let attempts = 0;
  const session = createGpuSession({ device: first,
    probe: { async acquire() { attempts++; return { device: null, reason: 'no adapter' }; } },
    createPresenter: () => () => {}, onFailure: reason => failures.push(reason) });
  for (let i = 0; i < 20; i++) first.uncapturederror({ error: { message: 'invalid draw' } });
  await tick();
  assert.equal(session.status(), 0);
  assert.equal(attempts, 1);
  assert.deepEqual(failures, ['no adapter']);
});
test('presenter binds a new offscreen texture after resize and submits a GPU pass', () => {
  let groups = 0, submits = 0, draws = 0;
  const pipeline = { getBindGroupLayout: () => ({}) };
  const d = { createShaderModule: x => { assert.equal(x.code, presenterWgsl); return x; }, createRenderPipeline: () => pipeline,
    createBindGroup() { groups++; return {}; }, queue: { submit() { submits++; } },
    createCommandEncoder: () => ({ finish: () => ({}), beginRenderPass: () => ({
      setPipeline() {}, setBindGroup() {}, draw(n) { assert.equal(n, 3); draws++; }, end() {} }) }) };
  const context = { configure(x) { assert.equal(x.device, d); assert.equal(x.alphaMode, 'opaque'); },
    getCurrentTexture: () => ({ createView: () => ({}) }) };
  const present = createGpuPresenter(d, { getContext(name) { assert.equal(name, 'webgpu'); return context; } },
    { getPreferredCanvasFormat: () => 'bgra8unorm' });
  const texture = { createView: () => ({}) };
  present(texture); present(texture); present({ createView: () => ({}) });
  assert.equal(groups, 2);
  assert.equal(submits, 3);
  assert.equal(draws, 3);
});

test('normal shutdown ignores device destruction and disposes a replacement arriving after stop', async () => {
  const first = device();
  let attempts = 0;
  const stopped = createGpuSession({ device: first, probe: { acquire() { attempts++; } },
    createPresenter: () => () => {}, onFailure() { assert.fail('shutdown must not report loss'); } });
  stopped.stop();
  first.lose({ message: 'normal shutdown' });
  await tick();
  assert.equal(attempts, 0);
  assert.equal(stopped.status(), 0);
  const next = device(), replacement = device(), pending = deferred();
  const recovering = createGpuSession({ device: next, probe: { acquire: () => pending.promise },
    createPresenter: () => () => {}, onFailure() { assert.fail('stopped session must remain stopped'); } });
  next.lose({ message: 'reset' });
  await tick();
  recovering.stop();
  pending.resolve({ device: replacement });
  await tick();
  assert.equal(replacement.destroyed, true);
  assert.equal(recovering.status(), 0);
});
