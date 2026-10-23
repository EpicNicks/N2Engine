// Tests web/n2-loader.js under node with a mock fetch. Usage: node scripts/web-loader-test.mjs
// The wasm module used is the smallest valid one (the 8 byte header), so nothing here needs a build.
// Covered: streaming instantiation with the right MIME type, the ArrayBuffer fallback with a wrong MIME type (on the
// same response: one request), a missing manifest, a missing manifest with a compressing host (Content-Length is the
// encoded size), a 404, a truncated file, a LinkError (index.js and index.wasm from different builds: no fallback), a
// page older than the manifest, monotonic progress against the manifest sizes, the cache-busting URLs.
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const N2Loader = createRequire(import.meta.url)(path.join(here, '..', 'web', 'n2-loader.js'));

const wasmBytes = new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]);
// A module that imports the function env.f: instantiating it with an env that lacks f is a LinkError, as an index.js
// of another build would cause
const wasmWithImport = new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
  1, 4, 1, 0x60, 0, 0,                          // type section: one type, () -> ()
  2, 9, 1, 3, 0x65, 0x6e, 0x76, 1, 0x66, 0, 0]); // import section: env.f, a function of type 0
const dataBytes = new Uint8Array(70000).map((_, i) => i % 251);
const hash = (c) => c.repeat(64);
const goodManifest = {
  files: {
    'index.wasm': { raw: wasmBytes.length, sha256: hash('a') },
    'index.data': { raw: dataBytes.length, sha256: hash('b') },
    'index.js': { raw: 100, sha256: hash('c') },
  },
};

// A mock host: url (as requested, with its query) -> Response. `wasmType` is what the host says .wasm is.
// `headers` adds response headers per file name (e.g. a Content-Encoding the mock host claims to have used).
function host({ wasmType = 'application/wasm', manifest = goodManifest, wasm = wasmBytes, data = dataBytes, status = {}, headers = {} } = {}) {
  const requests = [];
  const fetchMock = async (url, init) => {
    requests.push({ url, init });
    const name = url.split('?')[0];
    if (status[name]) return new Response('nope', { status: status[name], statusText: 'Mock' });
    if (name === 'manifest.json') {
      if (!manifest) return new Response('<html>not found</html>', { status: 200, headers: { 'content-type': 'text/html' } });
      return new Response(JSON.stringify(manifest), { headers: { 'content-type': 'application/json' } });
    }
    if (name === 'index.wasm') return new Response(wasm, { headers: { 'content-type': wasmType, ...headers[name] } });
    if (name === 'index.data') return new Response(data, { headers: { 'content-type': 'application/octet-stream', ...headers[name] } });
    return new Response('', { status: 404 });
  };
  return { fetchMock, requests };
}

// Plays Emscripten: asks the hooks for the pack and the wasm, the way the generated index.js does
function factoryWith(imports) {
  return (settings) => new Promise((resolve) => {
    const pack = settings.getPreloadedPackage(settings.locateFile('index.data'), dataBytes.length);
    settings.instantiateWasm(imports, (instance, module) => resolve({ pack, instance, module }));
  });
}
const fakeFactory = factoryWith({});

let failures = 0;
function check(condition, message) {
  if (!condition) {
    failures++;
    console.error(`FAIL: ${message}`);
  }
}

async function scenario(name, hostOptions, body, loaderOptions = {}) {
  const { fetchMock, requests } = host(hostOptions);
  const events = [];
  const progress = [];
  const loader = N2Loader.create({
    ...loaderOptions,
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
  check(requests.filter((r) => r.url.startsWith('index.wasm')).length === 1, 'the fallback reuses the first response: one request');
  check(!events.includes('wasm-streaming'), `no streaming attempt with a wrong type, events ${events}`);
});

await scenario('content type with parameters: still streams', { wasmType: 'application/wasm; charset=binary' }, async ({ loader, events }) => {
  const out = await loader.run(fakeFactory, {});
  check(out.instance instanceof WebAssembly.Instance, 'the wasm was instantiated');
  check(events.includes('wasm-streaming') && !events.some((e) => e.startsWith('wasm-fallback')), `events ${events}`);
});

await scenario('no manifest: still loads, unversioned urls, no total', { manifest: null }, async ({ loader, events, progress, requests }) => {
  const out = await loader.run(fakeFactory, {});
  check(out.instance instanceof WebAssembly.Instance, 'loaded without a manifest');
  check(events.some((e) => e.startsWith('manifest-missing')), `events ${events}`);
  check(requests.some((r) => r.url === 'index.wasm') && requests.some((r) => r.url === 'index.data'), 'urls are unversioned');
  check(progress.length > 0, 'progress is still reported');
});

// A compressing host (nginx gzip_static, the Apache .br rewrite): Content-Length is the encoded size, the body arrives
// decoded. Without a manifest the loader must not take Content-Length as the expected size.
const gzipped = (bytes) => ({ 'content-encoding': 'gzip', 'content-length': String(Math.floor(bytes.length / 3)) });
for (const wasmType of ['application/wasm', 'text/plain']) {
  await scenario(`no manifest, compressing host, wasm as ${wasmType}: loads, no length error`, {
    manifest: null, wasmType, headers: { 'index.wasm': gzipped(wasmBytes), 'index.data': gzipped(dataBytes) },
  }, async ({ loader, events, progress }) => {
    const out = await loader.run(fakeFactory, {});
    check(out.instance instanceof WebAssembly.Instance && out.pack.byteLength === dataBytes.length, 'loaded through a compressing host');
    check(events.includes('wasm-done') && events.includes('data-done'), `events ${events}`);
    check(progress.every((p) => p.total === 0), 'no total is claimed from an encoded Content-Length');
  });
}

await scenario('no manifest, plain host with Content-Length: a truncated pack is still caught', {
  manifest: null, data: dataBytes.slice(0, 1000), headers: { 'index.data': { 'content-length': String(dataBytes.length) } },
}, async ({ loader }) => {
  let message = null;
  await loader.run(fakeFactory, {}).catch((e) => { message = e.message; });
  check(message && message.includes('index.data') && message.includes(String(dataBytes.length)), `message was: ${message}`);
});

await scenario('LinkError (index.js of another build): a clear message, no fallback', { wasm: wasmWithImport, manifest: null }, async ({ loader, events, requests }) => {
  let message = null;
  await loader.run(factoryWith({ env: {} }), {}).catch((e) => { message = e.message; });
  check(message && message.includes('different builds') && message.includes('no-cache'), `message was: ${message}`);
  check(!events.some((e) => e.startsWith('wasm-fallback')), `no fallback for a LinkError, events ${events}`);
  check(requests.filter((r) => r.url.startsWith('index.wasm')).length === 1, 'index.wasm requested once');
});

await scenario('LinkError with a wrong MIME type: the same message', { wasm: wasmWithImport, wasmType: 'text/plain', manifest: null }, async ({ loader }) => {
  let message = null;
  await loader.run(factoryWith({ env: {} }), {}).catch((e) => { message = e.message; });
  check(message && message.includes('different builds'), `message was: ${message}`);
});

await scenario('page older than the manifest: reported before downloading', {}, async ({ loader, requests }) => {
  let message = null;
  let started = false;
  await loader.run(() => { started = true; }, {}).catch((e) => { message = e.message; });
  check(message && message.includes('older copy') && message.includes('Reload'), `message was: ${message}`);
  check(!started && !requests.some((r) => r.url.startsWith('index.data')), 'nothing else is downloaded for a stale page');
}, { indexJsVersion: 'f'.repeat(12) });

await scenario('page matching the manifest: loads', {}, async ({ loader }) => {
  const out = await loader.run(fakeFactory, {});
  check(out.instance instanceof WebAssembly.Instance, 'loaded');
}, { indexJsVersion: 'c'.repeat(12) });

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

// instantiateStreaming that fails after reading part of the body (and leaves its reader locked, as the browser does) for a
// reason that is not a module error: the loader reads the file again, and the first body must be cancelled, once
await scenario('streaming fails for another reason: the first body is cancelled once, the re-fetch loads', { manifest: null }, async ({ loader, events, requests, progress }) => {
  const realStreaming = WebAssembly.instantiateStreaming;
  let cancels = 0;
  let wasmRequests = 0;
  const base = host({ manifest: null }).fetchMock;
  const fetchMock = async (url, init) => {
    requests.push({ url, init });
    if (url.split('?')[0] !== 'index.wasm') return base(url, init);
    if (++wasmRequests > 1) return new Response(wasmBytes, { headers: { 'content-type': 'application/wasm' } });
    let sent = 0;
    const body = new ReadableStream({
      pull(controller) { controller.enqueue(wasmBytes.slice(sent, sent + 4)); sent += 4; if (sent >= wasmBytes.length * 5) controller.close(); },
      cancel() { cancels++; },
    });
    return new Response(body, { headers: { 'content-type': 'application/wasm' } });
  };
  const own = N2Loader.create({ fetch: fetchMock, onEvent: (e, d) => events.push(e + (d.reason ? `(${d.reason})` : '')), onProgress: (p) => progress.push(p) });
  WebAssembly.instantiateStreaming = async (response) => {
    const reader = response.body.getReader();
    await reader.read(); // part of the body, then it fails; the reader stays locked
    throw new TypeError('streaming compile failed');
  };
  try {
    const out = await own.run(fakeFactory, {});
    await new Promise((resolve) => setTimeout(resolve, 20));
    check(out.instance instanceof WebAssembly.Instance, 'the fallback loaded');
    check(events.some((e) => e.startsWith('wasm-fallback(streaming compile failed')), `events ${events}`);
    check(wasmRequests === 2, `the wasm is requested twice, got ${wasmRequests}`);
    check(cancels === 1, `the first body is cancelled exactly once, got ${cancels}`);
    check(progress.at(-1).loaded === wasmBytes.length + dataBytes.length, `no double count, ended at ${progress.at(-1)?.loaded}`);
  } finally {
    WebAssembly.instantiateStreaming = realStreaming;
  }
});

// Only a CompileError or LinkError means "different builds"; an out-of-memory RangeError keeps its own message
for (const [kind, make, isModule] of [
  ['CompileError', () => new WebAssembly.CompileError('bad module'), true],
  ['LinkError', () => new WebAssembly.LinkError('bad import'), true],
  ['RangeError (out of memory)', () => new RangeError('WebAssembly.instantiate(): Out of memory: Cannot allocate Wasm memory'), false],
]) {
  await scenario(`instantiate rejects with ${kind}: ${isModule ? 'moduleError' : 'passed through'}`, { wasmType: 'text/plain', manifest: null }, async ({ loader }) => {
    const realInstantiate = WebAssembly.instantiate;
    WebAssembly.instantiate = () => Promise.reject(make());
    let error = null;
    try {
      await loader.run(fakeFactory, {}).catch((e) => { error = e; });
    } finally {
      WebAssembly.instantiate = realInstantiate;
    }
    check(error && (error.message.includes('different builds') === isModule), `message was: ${error && error.message}`);
    check(error && (error.name === 'LoadError') === isModule, `error was: ${error && error.name}`);
    if (!isModule) check(error instanceof RangeError, 'the RangeError itself propagates');
  });
}

if (failures > 0) {
  console.error(`web loader test: ${failures} failure(s)`);
  process.exit(1);
}
console.log('web loader test: OK');
