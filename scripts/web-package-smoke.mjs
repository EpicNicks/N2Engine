// Checks the web package (the folder `cmake --build <dir> --target web-package` writes: <dir>/web-package) and runs
// it headless under node. Usage: node scripts/web-package-smoke.mjs <package folder> [frames] [WxH] [--precompressed]
//
// 1. The folder holds the static files and nothing else: index.html, index.js, index.wasm, index.data, n2-loader.js,
//    manifest.json (and, with --precompressed, the .br/.gz siblings of the five compressible files, which the size and
//    speed profiles write; without the flag there must be none), no worker script (a threaded build would need
//    cross-origin isolation), and the page refers to nothing by an absolute URL. manifest.json is checked against the
//    files: sizes, sha256, gzip and brotli sizes; every sibling must decompress to its plain file.
// 2. index.js is loaded the way the page loads it: through web/n2-loader.js (the package's copy) with a mock fetch that
//    serves the folder's files, serves index.wasm with the WRONG content type (text/plain), and so makes the loader
//    take its ArrayBuffer fallback on the same response (one request for index.wasm), which is asserted. The page's
//    script tags must name index.js and n2-loader.js with ?v=<their hash>. The player then runs N frames with the canvas mocked: it calls
//    Module.n2Present(pixels, width, height) for every frame, as it does for the page's canvas. The frames must come,
//    have the size asked for, and show something (more than one colour, some pixel not black). The loader's progress
//    must end at the manifest's sizes and the asset pack must have come through the loader's hook. Under node there is no
//    WebAudio: the player must run with the audio subsystem failed in its health (stage 4).
//
// This is not a browser: nothing here proves the canvas, the input events, the page or the hosting work. See
// docs/web-checklist.html for what a person has to check.
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
import crypto from 'node:crypto';
import zlib from 'node:zlib';

const positional = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const expectPrecompressed = process.argv.includes('--precompressed');
const dir = path.resolve(positional[0] ?? 'build-web/web-package');
const frames = Number(positional[1] ?? 30);
const size = positional[2] ?? '320x180';
const [width, height] = size.split('x').map(Number);
const timeoutMs = 120000;

const problems = [];
function expect(condition, message) {
  if (!condition) problems.push(message);
}

// 1. The package contents
const compressible = ['index.html', 'index.js', 'index.wasm', 'index.data', 'n2-loader.js'];
const required = [...compressible, 'manifest.json'];
const siblings = compressible.flatMap((name) => [`${name}.br`, `${name}.gz`]);
const present = fs.existsSync(dir) ? fs.readdirSync(dir).sort() : [];
console.log(`Package ${dir}: ${present.join(', ') || '(missing)'}`);
for (const name of required) {
  expect(present.includes(name), `missing ${name}`);
}
for (const name of expectPrecompressed ? siblings : []) {
  expect(present.includes(name), `missing ${name} (--precompressed)`);
}
for (const name of present) {
  const allowed = required.includes(name) || (expectPrecompressed && siblings.includes(name));
  expect(allowed, `unexpected file ${name} (the package is ${required.join(', ')}${expectPrecompressed ? ' and their .br/.gz siblings' : ''} only)`);
}
let manifest = null;
if (problems.length === 0) {
  // manifest.json against the files
  manifest = JSON.parse(fs.readFileSync(path.join(dir, 'manifest.json'), 'utf8'));
  expect(Object.keys(manifest.files).sort().join() === compressible.slice().sort().join(), `manifest lists ${Object.keys(manifest.files)}`);
  for (const name of compressible) {
    const entry = manifest.files[name];
    const plain = fs.readFileSync(path.join(dir, name));
    if (!entry) continue;
    expect(entry.raw === plain.length, `manifest: ${name} raw ${entry.raw} but the file is ${plain.length}`);
    expect(entry.sha256 === crypto.createHash('sha256').update(plain).digest('hex'), `manifest: sha256 of ${name} is wrong`);
    expect(entry.gzip === zlib.gzipSync(plain, { level: 9 }).length, `manifest: gzip size of ${name} is wrong`);
    if (expectPrecompressed) {
      const gz = fs.readFileSync(path.join(dir, `${name}.gz`));
      const br = fs.readFileSync(path.join(dir, `${name}.br`));
      expect(entry.gzip === gz.length && entry.brotli === br.length, `manifest: sizes of ${name} do not match its .gz/.br`);
      expect(zlib.gunzipSync(gz).equals(plain), `${name}.gz does not decompress to ${name}`);
      expect(zlib.brotliDecompressSync(br).equals(plain), `${name}.br does not decompress to ${name}`);
    }
  }
}
if (problems.length === 0) {
  const html = fs.readFileSync(path.join(dir, 'index.html'), 'utf8');
  // Every URL the page and the loader name is relative: no scheme, no leading slash
  // (emcc minifies the page at -O1 and up, which drops the quotes around attribute values: accept both)
  const urls = [...html.matchAll(/\b(?:src|href)\s*=\s*(?:["']([^"']+)["']|([^\s"'>]+))/g)].map((m) => m[1] ?? m[2]);
  for (const url of urls) {
    expect(!/^([a-z][a-z0-9+.-]*:|\/)/i.test(url), `index.html has a non-relative URL: ${url}`);
  }
  // Both scripts are loaded by a relative URL versioned with their own hash (scripts/web-package-finalize.mjs)
  for (const script of ['index.js', 'n2-loader.js']) {
    const versioned = `${script}?v=${manifest.files[script].sha256.slice(0, 12)}`;
    expect(urls.includes(versioned), `index.html does not load ${script} as ${versioned} (it names ${urls.join(', ')})`);
  }
  expect(fs.readFileSync(path.join(dir, 'n2-loader.js')).equals(fs.readFileSync(new URL('../web/n2-loader.js', import.meta.url))), 'the package n2-loader.js differs from web/n2-loader.js');
  expect(!/crossOriginIsolated|SharedArrayBuffer|Cross-Origin-/.test(html), 'index.html mentions cross-origin isolation');
  const js = fs.readFileSync(path.join(dir, 'index.js'), 'utf8');
  expect(!/PThread/.test(js), 'index.js looks like a threaded (pthreads) build');
  expect(js.includes('index.wasm') && js.includes('index.data'), 'index.js does not name index.wasm and index.data (relative)');
}

// 2. Run it
let frameCount = 0;
let lastFrame = null;
let badFrame = null;
const output = [];

function finish(code) {
  for (const problem of problems) console.error(`FAIL: ${problem}`);
  if (problems.length === 0) console.log('web package smoke: OK');
  process.exit(code);
}

if (problems.length > 0) {
  finish(1);
}

const done = new Promise((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error(`no result after ${timeoutMs / 1000} s`)), timeoutMs);
  globalThis.__n2Done = (framesRun) => { clearTimeout(timer); resolve(framesRun); };
  globalThis.__n2Failed = (message) => { clearTimeout(timer); reject(new Error(`the player failed: ${message}`)); };
});

// Emscripten's node loader opens the asset pack relative to the working directory: run from the package, as a server
// would serve it
process.chdir(dir);
const require = createRequire(pathToFileURL(path.join(dir, 'index.js')));
const createN2Player = require(path.join(dir, 'index.js'));
const N2Loader = require(path.join(dir, 'n2-loader.js'));
const gpuSelection = await N2Loader.createGpuProbe({ search: '' }).acquire();
expect(gpuSelection.renderer === 'software' && !gpuSelection.device,
  'node must select the software fallback without a browser WebGPU device');

// The mock host: serves the folder's files by the URL the loader asks for (query dropped), index.wasm with a wrong MIME type
const requested = [];
const loaderEvents = [];
let lastProgress = null;
async function mockFetch(url) {
  requested.push(url);
  const name = url.split('?')[0];
  if (!required.includes(name)) return new Response('not found', { status: 404 });
  const type = name === 'index.wasm' ? 'text/plain' : 'application/octet-stream';
  return new Response(fs.readFileSync(path.join(dir, name)), { headers: { 'content-type': type } });
}
const loader = N2Loader.create({
  fetch: mockFetch,
  indexJsVersion: manifest.files['index.js'].sha256.slice(0, 12), // as the page passes it: this page is current
  onEvent: (event) => loaderEvents.push(event),
  onProgress: (p) => { lastProgress = p; },
});
try {
  loader.run(createN2Player, {
    arguments: ['--frames', String(frames), '--size', size],
    print: (text) => { output.push(text); console.log(text); },
    printErr: (text) => {
      output.push(text);
      console.error(text);
      if (text.startsWith('package error')) globalThis.__n2Failed(text); // the asset pack did not load: no point waiting
    },
    // The canvas, mocked: keeps the last frame and checks the shape of each
    n2Present: (pixels, w, h) => {
      ++frameCount;
      if (w !== width || h !== height || pixels.length !== w * h * 4) {
        badFrame ??= `frame ${frameCount} is ${w}x${h} with ${pixels.length} bytes, expected ${width}x${height}`;
      }
      lastFrame = Uint8Array.from(pixels); // the view is of wasm memory, which the next frame overwrites
    },
    n2Stopped: (framesRun) => globalThis.__n2Done(framesRun),
    n2Failed: (message) => globalThis.__n2Failed(message),
    onAbort: (reason) => globalThis.__n2Failed(`abort: ${reason}`),
  }).catch((error) => globalThis.__n2Failed(String(error?.message ?? error)));
  const framesRun = await done;
  expect(framesRun === frames, `the player ran ${framesRun} frames, expected ${frames}`);
} catch (error) {
  problems.push(String(error?.message ?? error));
}

// The loader's part: manifest read, MIME fallback taken, the pack and the wasm fetched by hash, progress complete
expect(loaderEvents.includes('manifest-ok'), `loader events: ${loaderEvents}`);
expect(loaderEvents.includes('wasm-fallback') && loaderEvents.includes('wasm-done'), `the wrong MIME type did not take the ArrayBuffer fallback: ${loaderEvents}`);
expect(loaderEvents.includes('data-done'), `the asset pack did not come through the loader: ${loaderEvents}`);
if (manifest) {
  for (const name of ['index.wasm', 'index.data']) {
    expect(requested.includes(`${name}?v=${manifest.files[name].sha256.slice(0, 12)}`), `${name} was not requested by its hash: ${requested}`);
  }
  const wasmGets = requested.filter((url) => url.split('?')[0] === 'index.wasm').length;
  expect(wasmGets === 1, `index.wasm was requested ${wasmGets} times, expected once (the fallback reuses the first response)`);
  const total = manifest.files['index.wasm'].raw + manifest.files['index.data'].raw;
  expect(lastProgress && lastProgress.loaded === total && lastProgress.total === total, `loader progress ended at ${JSON.stringify(lastProgress)}, expected ${total}`);
}
console.log(`Loader: ${loaderEvents.join(', ')}`);
expect(frameCount === frames, `${frameCount} frames reached the canvas, expected ${frames}`);
expect(!badFrame, badFrame ?? '');
expect(output.some((line) => line.startsWith('N2_WEB_SMOKE_DONE')), 'the player did not print N2_WEB_SMOKE_DONE');
// Audio (stage 4): node has no WebAudio, so the player must start without sound (the frames above prove it ran, did not
// crash and did not hang) and its health must report the audio subsystem as failed, not running or missing. The engine's log
// does not reach this output, so the one warning itself is not counted here.
const audioLines = output.filter((line) => line.startsWith('N2_WEB_AUDIO'));
expect(audioLines.length === 1 && audioLines[0] === 'N2_WEB_AUDIO state=failed', `expected 'N2_WEB_AUDIO state=failed' under node (no WebAudio), got ${JSON.stringify(audioLines)}`);
if (lastFrame) {
  const colours = new Set();
  let notBlack = 0;
  for (let i = 0; i < lastFrame.length; i += 4) {
    const rgb = (lastFrame[i] << 16) | (lastFrame[i + 1] << 8) | lastFrame[i + 2];
    if (rgb !== 0) ++notBlack;
    if (colours.size < 64) colours.add(rgb);
  }
  console.log(`Last frame: ${width}x${height}, ${notBlack} pixels not black, at least ${colours.size} colours`);
  expect(notBlack > 0, 'the last frame is all black');
  expect(colours.size > 1, 'the last frame has a single colour');
} else {
  problems.push('no frame reached the canvas');
}
finish(problems.length === 0 ? 0 : 1);
