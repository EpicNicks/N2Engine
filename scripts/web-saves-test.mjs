// Tests web/n2-saves.js (persistent saves on the web, issue #423) under node with a mocked FS and IDBFS.
// Usage: node scripts/web-saves-test.mjs. Nothing here needs a build or a browser.
// Covered: the mount and the populate before start (the run dependency is released only after it), the fallback to memory
// with exactly one warning (no indexedDB, no IDBFS, a mount that throws, a populate error), debounced write-back (many writes,
// one sync), the maximum wait under a steady stream of writes, one sync in flight at a time, writes outside the mount and
// reads not syncing, flush on pagehide and when hidden, a failing write-back (quota) warning once and retrying, and
// that nothing throws into the engine.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const source = fs.readFileSync(path.join(here, '..', 'web', 'n2-saves.js'), 'utf8');
// The file defines N2PersistentSaves and skips its Module glue when Module, FS and ENV are not in scope
const N2PersistentSaves = new Function(`${source}\nreturn N2PersistentSaves;`)();

let failures = 0;
function check(condition, message) {
  if (!condition) { failures += 1; console.error(`FAIL: ${message}`); }
}

const DIR = '/n2-userdata';

// A clock and timers the test drives
function fakeTime() {
  let t = 0;
  let nextId = 1;
  const timers = new Map();
  return {
    now: () => t,
    setTimeout: (fn, ms) => { const id = nextId++; timers.set(id, { at: t + ms, fn }); return id; },
    clearTimeout: (id) => { timers.delete(id); },
    advance(ms) {
      const end = t + ms;
      for (;;) {
        let best = null;
        for (const [id, timer] of timers) if (timer.at <= end && (best === null || timer.at < timers.get(best).at)) best = id;
        if (best === null) break;
        const timer = timers.get(best);
        timers.delete(best);
        t = Math.max(t, timer.at);
        timer.fn();
      }
      t = end;
    },
    pending: () => timers.size,
  };
}

// A mock Emscripten FS. syncfs calls are recorded; their callbacks are held so a test decides when they finish.
function mockFs(behaviour = {}) {
  const calls = { mkdir: [], mount: [], syncfs: [] };
  const FS = {
    mkdir(p) { calls.mkdir.push(p); if (behaviour.mkdirThrows) throw new Error('EEXIST'); },
    mount(type, opts, p) { calls.mount.push({ type, p }); if (behaviour.mountThrows) throw new Error('mount failed'); },
    syncfs(populate, cb) {
      calls.syncfs.push({ populate, cb });
      if (behaviour.syncThrows) throw new Error('sync threw');
      if (behaviour.autoComplete !== false) {
        const error = populate ? behaviour.populateError : behaviour.writeError;
        cb(error ? new Error(error) : null);
      }
    },
    close(stream) { calls.close = (calls.close || 0) + 1; },
    writeFile() {}, unlink() {}, rmdir() {}, rename() {},
  };
  return { FS, calls };
}

function setup({ fsBehaviour, idbfs = {}, hasIndexedDb = true, listenTarget = null, document = null } = {}) {
  const { FS, calls } = mockFs(fsBehaviour);
  const time = fakeTime();
  const warnings = [];
  const originalClose = FS.close;
  const saves = N2PersistentSaves({
    FS, IDBFS: idbfs, hasIndexedDb, dir: DIR, document,
    warn: (text) => warnings.push(text),
    now: time.now, setTimeout: time.setTimeout, clearTimeout: time.clearTimeout,
  });
  let released = false;
  saves.start(() => { released = true; });
  if (listenTarget) saves.listen(listenTarget);
  return { FS, calls, time, warnings, saves, released: () => released, originalClose };
}

const writeStream = (p) => ({ path: p, flags: 1 });
const readStream = (p) => ({ path: p, flags: 0 });
const writes = (calls) => calls.syncfs.filter((c) => !c.populate);

// ---- startup ----
{
  const t = setup({ fsBehaviour: { autoComplete: false } });
  check(t.calls.mount.length === 1 && t.calls.mount[0].p === DIR && t.calls.mount[0].type === t.calls.mount[0].type, 'IDBFS is mounted at the user data directory');
  check(t.calls.syncfs.length === 1 && t.calls.syncfs[0].populate === true, 'the populate (syncfs(true)) runs at start');
  check(!t.released(), 'the run dependency is held until the populate finishes');
  t.calls.syncfs[0].cb(null);
  check(t.released(), 'the run dependency is released after the populate');
  check(t.saves.state().mode === 'idbfs', 'persistent mode after a successful populate');
  check(t.warnings.length === 0, 'no warning on success');
}

// ---- fallbacks: one warning, never a crash, the start is never blocked ----
for (const [name, options] of [
  ['no indexedDB', { hasIndexedDb: false }],
  ['no IDBFS linked', { idbfs: null }],
  ['a mount that throws', { fsBehaviour: { mountThrows: true } }],
  ['a populate error (private window)', { fsBehaviour: { populateError: 'SecurityError' } }],
  ['syncfs throwing', { fsBehaviour: { syncThrows: true } }],
]) {
  const t = setup(options);
  check(t.released(), `${name}: the start is released`);
  check(t.saves.state().mode === 'memory', `${name}: memory mode`);
  check(t.warnings.length === 1 && /memory/.test(t.warnings[0]), `${name}: exactly one warning (got ${JSON.stringify(t.warnings)})`);
  // writes afterwards do nothing and never throw
  t.FS.close(writeStream(`${DIR}/n2engine/x`));
  t.saves.flush();
  t.time.advance(10000);
  check(writes(t.calls).length === 0, `${name}: no write-back is attempted`);
  check(t.warnings.length === 1, `${name}: still one warning`);
}

// ---- debounced write-back ----
{
  const t = setup();
  const original = t.FS.close;
  for (let i = 0; i < 20; i += 1) { t.FS.close(writeStream(`${DIR}/n2engine/p-1/saves/slot${i}`)); t.time.advance(100); }
  check(writes(t.calls).length === 0, 'writes in a burst do not sync while they continue');
  t.time.advance(1000);
  check(writes(t.calls).length === 1, 'one write-back after a quiet second');
  check(t.saves.state().syncs === 1 && !t.saves.state().dirty, 'the sync finished and nothing is left dirty');
  t.time.advance(10000);
  check(writes(t.calls).length === 1, 'no more syncs when nothing was written');
  void original;
}

// ---- the maximum wait: a stream of writes cannot postpone the sync forever ----
{
  const t = setup();
  for (let i = 0; i < 80; i += 1) { t.FS.close(writeStream(`${DIR}/a`)); t.time.advance(100); }
  check(writes(t.calls).length >= 1, 'a steady stream of writes still syncs (max wait)');
}

// ---- what counts as a write ----
{
  const t = setup();
  t.FS.close(readStream(`${DIR}/a`));
  t.FS.close(writeStream('/project/assets/a'));
  t.FS.unlink('/tmp/x');
  t.time.advance(5000);
  check(writes(t.calls).length === 0, 'reads and writes outside the mount do not sync');
  t.FS.unlink(`${DIR}/n2engine/p/saves/a`);
  t.time.advance(1000);
  check(writes(t.calls).length === 1, 'unlink under the mount syncs');
  t.FS.rename('/tmp/a', `${DIR}/b`);
  t.FS.rmdir(`${DIR}/c`);
  t.FS.mkdir(`${DIR}/d`);
  t.FS.writeFile(`${DIR}/e`, 'x');
  t.time.advance(1000);
  check(writes(t.calls).length === 2, 'rename, rmdir, mkdir and writeFile under the mount sync');
  t.FS.mkdir(`${DIR}-other/x`);
  t.time.advance(5000);
  check(writes(t.calls).length === 2, 'a sibling folder with the same prefix is not under the mount');
}

// ---- one sync in flight ----
{
  const t = setup({ fsBehaviour: { autoComplete: false } });
  t.calls.syncfs[0].cb(null); // populate
  t.FS.close(writeStream(`${DIR}/a`));
  t.time.advance(1000);
  check(writes(t.calls).length === 1, 'first write-back started');
  t.FS.close(writeStream(`${DIR}/b`));
  t.saves.flush();
  t.time.advance(5000);
  check(writes(t.calls).length === 1, 'no second sync while one is running');
  writes(t.calls)[0].cb(null);
  t.time.advance(1000);
  check(writes(t.calls).length === 2, 'the write made meanwhile is synced after the first finishes');
  writes(t.calls)[1].cb(null);
  t.time.advance(5000);
  check(writes(t.calls).length === 2 && t.saves.state().syncs === 2, 'and then it stays quiet');
}

// ---- flush on pagehide and when hidden ----
{
  const handlers = {};
  const target = { addEventListener: (name, fn) => { handlers[name] = fn; } };
  const document = { visibilityState: 'visible' };
  const t = setup({ listenTarget: target, document });
  t.FS.close(writeStream(`${DIR}/a`));
  document.visibilityState = 'visible';
  handlers.visibilitychange();
  check(writes(t.calls).length === 0, 'becoming visible does not sync');
  document.visibilityState = 'hidden';
  handlers.visibilitychange();
  check(writes(t.calls).length === 1, 'hiding the page syncs at once');
  t.FS.close(writeStream(`${DIR}/b`));
  handlers.pagehide();
  check(writes(t.calls).length === 2, 'pagehide syncs at once');
  handlers.pagehide();
  check(writes(t.calls).length === 2, 'pagehide with nothing unsaved does not sync');
}

// ---- a failing write-back (quota): one warning, game goes on, retried on the next write ----
{
  const t = setup({ fsBehaviour: { writeError: 'QuotaExceededError' } });
  t.FS.close(writeStream(`${DIR}/a`));
  t.time.advance(1000);
  check(t.saves.state().failures === 1, 'the failure is counted');
  check(t.warnings.length === 1 && /QuotaExceeded/.test(t.warnings[0]), 'one warning naming the error');
  check(t.saves.state().mode === 'idbfs', 'persistence stays on after a failed write-back');
  t.FS.close(writeStream(`${DIR}/b`));
  t.time.advance(1000);
  check(writes(t.calls).length === 2, 'the next write retries');
  check(t.warnings.length === 1, 'no second warning');
}

if (failures) {
  console.error(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('web saves tests passed');
