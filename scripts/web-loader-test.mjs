// Tests web/n2-loader.js under node with a mock fetch. Usage: node scripts/web-loader-test.mjs
// The wasm module used is the smallest valid one (the 8 byte header), so nothing here needs a build.
// Covered: streaming instantiation with the right MIME type, the ArrayBuffer fallback with a wrong MIME type, a missing
// manifest, a 404, a truncated file, monotonic progress against the manifest sizes, the cache-busting URLs.
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const N2Loader = createRequire(import.meta.url)(path.join(here, '..', 'web', 'n2-loader.js'));

const wasmBytes = new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]);
const dataBytes = new Uint8Array(70000).map((_, i) => i % 251);
const hash = (c) => c.repeat(64);
const goodManifest = {
  files: {
    'index.wasm': { raw: wasmBytes.length, sha256: hash('a') },
    'index.data': { raw: dataBytes.length, sha256: hash('b') },
  },
};

// A mock host: url (as requested, with its query) -> Response. `wasmType` is what the host says .wasm is.
function host({ wasmType = 'application/wasm', manifest = goodManifest, wasm = wasmBytes, data = dataBytes, status = {} } = {}) {
  const requests = [];
  const fetchMock = async (url, init) => {
    requests.push({ url, init });
    const name = url.split('?')[0];
    if (status[name]) return new Response('nope', { status: status[name], statusText: 'Mock' });
    if (name === 'manifest.json') {
      if (!manifest) return new Response('<html>not found</html>', { status: 200, headers: { 'content-type': 'text/html' } });
      return new Response(JSON.stringify(manifest), { headers: { 'content-type': 'application/json' } });
    }
    if (name === 'index.wasm') return new Response(wasm, { headers: { 'content-type': wasmType } });
    if (name === 'index.data') return new Response(data, { headers: { 'content-type': 'application/octet-stream' } });
    return new Response('', { status: 404 });
  };
  return { fetchMock, requests };
}

// Plays Emscripten: asks the hooks for the pack and the wasm, the way the generated index.js does
function fakeFactory(settings) {
  return new Promise((resolve) => {
    const pack = settings.getPreloadedPackage(settings.locateFile('index.data'), dataBytes.length);
    settings.instantiateWasm({}, (instance, module) => resolve({ pack, instance, module }));
  });
}

let failures = 0;
function check(condition, message) {
  if (!condition) {
    failures++;
    console.error(`FAIL: ${message}`);
  }
}

async function scenario(name, hostOptions, body) {
  const { fetchMock, requests } = host(hostOptions);
  const events = [];
  const progress = [];
  const loader = N2Loader.create({
    fetch: fetchMock,
    onEvent: (event, detail) => events.push(event + (detail.reason ? `(${detail.reason})` : '')),
    onProgress: (p) => progress.push(p),
  });
  try {
    await body({ loader, events, progress, requests });
    console.log(`ok: ${name}`);
  } catch (error) {
    failures++;
    console.error(`FAIL: ${name}: ${error?.stack ?? error}`);
  }
}

await scenario('right MIME type: streaming, no fallback, versioned urls, progress', {}, async ({ loader, events, progress, requests }) => {
  const out = await loader.run(fakeFactory, {});
  check(out.pack && out.pack.byteLength === dataBytes.length, 'the pack was handed over whole');
  check(out.instance instanceof WebAssembly.Instance, 'the wasm was instantiated');
  check(events.includes('manifest-ok') && events.includes('wasm-streaming'), `events ${events}`);
  check(!events.some((e) => e.startsWith('wasm-fallback')), `no fallback expected, events ${events}`);
  check(requests[0].url === 'manifest.json' && requests[0].init?.cache === 'no-cache', 'manifest fetched first with no-cache');
  check(requests.some((r) => r.url === `index.data?v=${'b'.repeat(12)}`), 'the pack is requested by its hash');
  check(requests.some((r) => r.url === `index.wasm?v=${'a'.repeat(12)}`), 'the wasm is requested by its hash');
  const total = wasmBytes.length + dataBytes.length;
  check(progress.length > 0 && progress.every((p) => p.total === total), 'progress totals come from the manifest');
  check(progress.every((p, i) => i === 0 || p.loaded >= progress[i - 1].loaded), 'progress never goes backwards');
  check(progress.at(-1).loaded === total, `progress ends at the total (${progress.at(-1)?.loaded} of ${total})`);
});

await scenario('wrong MIME type: falls back to ArrayBuffer instantiation', { wasmType: 'text/plain' }, async ({ loader, events, requests }) => {
  const out = await loader.run(fakeFactory, {});
  check(out.instance instanceof WebAssembly.Instance, 'the wasm was instantiated through the fallback');
  check(events.some((e) => e.startsWith('wasm-fallback')), `fallback expected, events ${events}`);
  check(requests.filter((r) => r.url.startsWith('index.wasm')).length === 2, 'the wasm is fetched a second time for the fallback');
});

await scenario('no manifest: still loads, unversioned urls, no total', { manifest: null }, async ({ loader, events, progress, requests }) => {
  const out = await loader.run(fakeFactory, {});
  check(out.instance instanceof WebAssembly.Instance, 'loaded without a manifest');
  check(events.some((e) => e.startsWith('manifest-missing')), `events ${events}`);
  check(requests.some((r) => r.url === 'index.wasm') && requests.some((r) => r.url === 'index.data'), 'urls are unversioned');
  check(progress.length > 0, 'progress is still reported');
});

await scenario('404 on the wasm: a clear error', { status: { 'index.wasm': 404 } }, async ({ loader }) => {
  let message = null;
  await loader.run(fakeFactory, {}).catch((e) => { message = e.message; });
  check(message && message.includes('index.wasm') && message.includes('404'), `message was: ${message}`);
});

await scenario('404 on the pack: a clear error before the engine starts', { status: { 'index.data': 404 } }, async ({ loader }) => {
  let message = null;
  let started = false;
  await loader.run(() => { started = true; return fakeFactory; }, {}).catch((e) => { message = e.message; });
  check(message && message.includes('index.data') && message.includes('404'), `message was: ${message}`);
  check(!started, 'the engine is not started without its assets');
});

await scenario('truncated pack: caught against the manifest size', { data: dataBytes.slice(0, 1000) }, async ({ loader }) => {
  let message = null;
  await loader.run(fakeFactory, {}).catch((e) => { message = e.message; });
  check(message && message.includes('index.data') && message.includes('1000'), `message was: ${message}`);
});

await scenario('network failure: a clear error', {}, async () => {
  const loader = N2Loader.create({ fetch: async () => { throw new TypeError('Failed to fetch'); } });
  const manifestOk = loader.manifest() === null; // nothing fetched yet
  let message = null;
  await loader.run(fakeFactory, {}).catch((e) => { message = e.message; });
  check(manifestOk && message && message.includes('index.data') && message.includes('network'), `message was: ${message}`);
});

if (failures > 0) {
  console.error(`web loader test: ${failures} failure(s)`);
  process.exit(1);
}
console.log('web loader test: OK');
