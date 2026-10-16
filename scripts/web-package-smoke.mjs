// Checks the web package (the folder `cmake --build <dir> --target web-package` writes: <dir>/web-package) and runs
// it headless under node. Usage: node scripts/web-package-smoke.mjs <package folder> [frames] [WxH]
//
// 1. The folder holds the static files and nothing else: index.html, index.js, index.wasm, index.data, no worker script
//    (a threaded build would need cross-origin isolation), and the page refers to nothing by an absolute URL.
// 2. index.js is loaded the way the page loads it (createN2Player(settings)) and run for N frames with the canvas mocked:
//    the player calls Module.n2Present(pixels, width, height) for every frame, as it does for the page's canvas. The
//    frames must come, have the size asked for, and show something (more than one colour, some pixel not black).
//
// This is not a browser: nothing here proves the canvas, the input events, the page or the hosting work. See
// docs/web-checklist.html for what a person has to check.
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';

const dir = path.resolve(process.argv[2] ?? 'build-web/web-package');
const frames = Number(process.argv[3] ?? 30);
const size = process.argv[4] ?? '320x180';
const [width, height] = size.split('x').map(Number);
const timeoutMs = 120000;

const problems = [];
function expect(condition, message) {
  if (!condition) problems.push(message);
}

// 1. The package contents
const required = ['index.html', 'index.js', 'index.wasm', 'index.data'];
const present = fs.existsSync(dir) ? fs.readdirSync(dir).sort() : [];
console.log(`Package ${dir}: ${present.join(', ') || '(missing)'}`);
for (const name of required) {
  expect(present.includes(name), `missing ${name}`);
}
for (const name of present) {
  expect(required.includes(name), `unexpected file ${name} (the package is index.html, index.js, index.wasm and index.data only)`);
}
if (problems.length === 0) {
  const html = fs.readFileSync(path.join(dir, 'index.html'), 'utf8');
  // Every URL the page and the loader name is relative: no scheme, no leading slash
  const urls = [...html.matchAll(/\b(?:src|href)\s*=\s*["']([^"']+)["']/g)].map((m) => m[1]);
  for (const url of urls) {
    expect(!/^([a-z][a-z0-9+.-]*:|\/)/i.test(url), `index.html has a non-relative URL: ${url}`);
  }
  expect(urls.includes('index.js'), 'index.html does not load index.js by a relative URL');
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
try {
  createN2Player({
    arguments: ['--frames', String(frames), '--size', size],
    print: (text) => { output.push(text); console.log(text); },
    printErr: (text) => {
      output.push(text);
      console.error(text);
      if (text.startsWith('package error')) globalThis.__n2Failed(text); // the asset pack did not load: no point waiting
    },
    locateFile: (name) => path.join(dir, name),
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

expect(frameCount === frames, `${frameCount} frames reached the canvas, expected ${frames}`);
expect(!badFrame, badFrame ?? '');
expect(output.some((line) => line.startsWith('N2_WEB_SMOKE_DONE')), 'the player did not print N2_WEB_SMOKE_DONE');
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
